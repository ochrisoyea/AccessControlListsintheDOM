// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.locale;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.StrictMode;
import android.support.annotation.IntDef;
import android.support.annotation.Nullable;

import org.chromium.base.ActivityState;
import org.chromium.base.ApiCompatibilityUtils;
import org.chromium.base.ApplicationStatus;
import org.chromium.base.Callback;
import org.chromium.base.ContextUtils;
import org.chromium.base.ThreadUtils;
import org.chromium.base.VisibleForTesting;
import org.chromium.base.annotations.CalledByNative;
import org.chromium.base.library_loader.LibraryLoader;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.AppHooks;
import org.chromium.chrome.browser.ChromeFeatureList;
import org.chromium.chrome.browser.preferences.PreferencesLauncher;
import org.chromium.chrome.browser.preferences.SearchEnginePreference;
import org.chromium.chrome.browser.search_engines.TemplateUrl;
import org.chromium.chrome.browser.search_engines.TemplateUrlService;
import org.chromium.chrome.browser.snackbar.Snackbar;
import org.chromium.chrome.browser.snackbar.SnackbarManager;
import org.chromium.chrome.browser.snackbar.SnackbarManager.SnackbarController;
import org.chromium.chrome.browser.vr_shell.OnExitVrRequestListener;
import org.chromium.chrome.browser.vr_shell.VrIntentUtils;
import org.chromium.chrome.browser.vr_shell.VrShellDelegate;
import org.chromium.chrome.browser.widget.PromoDialog;
import org.chromium.ui.base.PageTransition;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.ref.WeakReference;
import java.util.List;
import java.util.concurrent.Callable;

/**
 * Manager for some locale specific logics.
 */
public class LocaleManager {
    public static final String PREF_AUTO_SWITCH = "LocaleManager_PREF_AUTO_SWITCH";
    public static final String PREF_PROMO_SHOWN = "LocaleManager_PREF_PROMO_SHOWN";
    public static final String PREF_WAS_IN_SPECIAL_LOCALE = "LocaleManager_WAS_IN_SPECIAL_LOCALE";
    public static final String SPECIAL_LOCALE_ID = "US";

    /** The current state regarding search engine promo dialogs. */
    @IntDef({SEARCH_ENGINE_PROMO_SHOULD_CHECK, SEARCH_ENGINE_PROMO_CHECKED_NOT_SHOWN,
            SEARCH_ENGINE_PROMO_CHECKED_AND_SHOWN})
    @Retention(RetentionPolicy.SOURCE)
    public @interface SearchEnginePromoState {}
    public static final int SEARCH_ENGINE_PROMO_SHOULD_CHECK = -1;
    public static final int SEARCH_ENGINE_PROMO_CHECKED_NOT_SHOWN = 0;
    public static final int SEARCH_ENGINE_PROMO_CHECKED_AND_SHOWN = 1;

    /** The different types of search engine promo dialogs. */
    @IntDef({SEARCH_ENGINE_PROMO_DONT_SHOW, SEARCH_ENGINE_PROMO_SHOW_SOGOU,
            SEARCH_ENGINE_PROMO_SHOW_EXISTING, SEARCH_ENGINE_PROMO_SHOW_NEW})
    @Retention(RetentionPolicy.SOURCE)
    public @interface SearchEnginePromoType {}

    public static final int SEARCH_ENGINE_PROMO_DONT_SHOW = -1;
    public static final int SEARCH_ENGINE_PROMO_SHOW_SOGOU = 0;
    public static final int SEARCH_ENGINE_PROMO_SHOW_EXISTING = 1;
    public static final int SEARCH_ENGINE_PROMO_SHOW_NEW = 2;

    protected static final String KEY_SEARCH_ENGINE_PROMO_SHOW_STATE =
            "com.android.chrome.SEARCH_ENGINE_PROMO_SHOWN";

    private static final int SNACKBAR_DURATION_MS = 6000;

    private static LocaleManager sInstance;

    private boolean mSearchEnginePromoCompleted;
    private boolean mSearchEnginePromoShownThisSession;
    private boolean mSearchEnginePromoCheckedThisSession;

    // LocaleManager is a singleton and it should not have strong reference to UI objects.
    // SnackbarManager is owned by ChromeActivity and is not null as long as the activity is alive.
    private WeakReference<SnackbarManager> mSnackbarManager;
    private LocaleTemplateUrlLoader mLocaleTemplateUrlLoader;

    private SnackbarController mSnackbarController = new SnackbarController() {
        @Override
        public void onDismissNoAction(Object actionData) { }

        @Override
        public void onAction(Object actionData) {
            Context context = ContextUtils.getApplicationContext();
            Intent intent = PreferencesLauncher.createIntentForSettingsPage(context,
                    SearchEnginePreference.class.getName());
            context.startActivity(intent);
        }
    };

    /**
     * @return An instance of the {@link LocaleManager}. This should only be called on UI thread.
     */
    @CalledByNative
    public static LocaleManager getInstance() {
        assert ThreadUtils.runningOnUiThread();
        if (sInstance == null) {
            sInstance = AppHooks.get().createLocaleManager();
        }
        return sInstance;
    }

    /**
     * Default constructor.
     */
    public LocaleManager() {
        int state = SEARCH_ENGINE_PROMO_SHOULD_CHECK;
        StrictMode.ThreadPolicy oldPolicy = StrictMode.allowThreadDiskReads();
        try {
            state = ContextUtils.getAppSharedPreferences().getInt(
                    KEY_SEARCH_ENGINE_PROMO_SHOW_STATE, SEARCH_ENGINE_PROMO_SHOULD_CHECK);
        } finally {
            StrictMode.setThreadPolicy(oldPolicy);
        }
        mSearchEnginePromoCompleted = state == SEARCH_ENGINE_PROMO_CHECKED_AND_SHOWN;
    }

    /**
     * Starts listening to state changes of the phone.
     */
    public void startObservingPhoneChanges() {
        maybeAutoSwitchSearchEngine();
    }

    /**
     * Stops listening to state changes of the phone.
     */
    public void stopObservingPhoneChanges() {}

    /**
     * Starts recording metrics in deferred startup.
     */
    public void recordStartupMetrics() {}

    /**
     * @return Whether the Chrome instance is running in a special locale.
     */
    public boolean isSpecialLocaleEnabled() {
        // If there is a kill switch sent from the server, disable the feature.
        if (!ChromeFeatureList.isEnabled("SpecialLocaleWrapper")) {
            return false;
        }
        boolean inSpecialLocale = ChromeFeatureList.isEnabled("SpecialLocale");
        inSpecialLocale = isReallyInSpecialLocale(inSpecialLocale);
        return inSpecialLocale;
    }

    /**
     * @return The country id of the special locale.
     */
    public String getSpecialLocaleId() {
        return SPECIAL_LOCALE_ID;
    }

    /**
     * Adds local search engines for special locale.
     */
    public void addSpecialSearchEngines() {
        if (!isSpecialLocaleEnabled()) return;
        getLocaleTemplateUrlLoader().loadTemplateUrls();
    }

    /**
     * Removes local search engines for special locale.
     */
    public void removeSpecialSearchEngines() {
        if (isSpecialLocaleEnabled()) return;
        getLocaleTemplateUrlLoader().removeTemplateUrls();
    }

    /**
     * Overrides the default search engine to a different search engine we designate. This is a
     * no-op if the user has manually changed DSP settings.
     */
    public void overrideDefaultSearchEngine() {
        if (!isSearchEngineAutoSwitchEnabled() || !isSpecialLocaleEnabled()) return;
        getLocaleTemplateUrlLoader().overrideDefaultSearchProvider();
        showSnackbar(ContextUtils.getApplicationContext().getString(R.string.using_sogou));
    }

    /**
     * Reverts the temporary change made in {@link #overrideDefaultSearchEngine()}. This is a no-op
     * if the user has manually changed DSP settings.
     */
    public void revertDefaultSearchEngineOverride() {
        if (!isSearchEngineAutoSwitchEnabled() || isSpecialLocaleEnabled()) return;
        getLocaleTemplateUrlLoader().setGoogleAsDefaultSearch();
        showSnackbar(ContextUtils.getApplicationContext().getString(R.string.using_google));
    }

    /**
     * Switches the default search engine based on the current locale, if the user has delegated
     * Chrome to do so. This method also adds some special engines to user's search engine list, as
     * long as the user is in this locale.
     */
    protected void maybeAutoSwitchSearchEngine() {
        SharedPreferences preferences = ContextUtils.getAppSharedPreferences();
        boolean wasInSpecialLocale = preferences.getBoolean(PREF_WAS_IN_SPECIAL_LOCALE, false);
        boolean isInSpecialLocale = isSpecialLocaleEnabled();
        if (wasInSpecialLocale && !isInSpecialLocale) {
            revertDefaultSearchEngineOverride();
            removeSpecialSearchEngines();
        } else if (isInSpecialLocale && !wasInSpecialLocale) {
            addSpecialSearchEngines();
            overrideDefaultSearchEngine();
        } else if (isInSpecialLocale) {
            // As long as the user is in the special locale, special engines should be in the list.
            addSpecialSearchEngines();
        }
        preferences.edit().putBoolean(PREF_WAS_IN_SPECIAL_LOCALE, isInSpecialLocale).apply();
    }

    /**
     * Shows a promotion dialog about search engines depending on Locale and other conditions.
     * See {@link LocaleManager#getSearchEnginePromoShowType()} for possible types and logic.
     *
     * @param activity    Activity showing the dialog.
     * @param onSearchEngineFinalized Notified when the search engine has been finalized.  This can
     *                                either mean no dialog is needed, or the dialog was needed and
     *                                the user completed the dialog with a valid selection.
     */
    public void showSearchEnginePromoIfNeeded(
            final Activity activity, final @Nullable Callback<Boolean> onSearchEngineFinalized) {
        assert LibraryLoader.isInitialized();
        TemplateUrlService.getInstance().runWhenLoaded(new Runnable() {
            @Override
            public void run() {
                handleSearchEnginePromoWithTemplateUrlsLoaded(activity, onSearchEngineFinalized);
            }
        });
    }

    private void handleSearchEnginePromoWithTemplateUrlsLoaded(
            final Activity activity, final @Nullable Callback<Boolean> onSearchEngineFinalized) {
        assert TemplateUrlService.getInstance().isLoaded();

        final Callback<Boolean> finalizeInternalCallback = new Callback<Boolean>() {
            @Override
            public void onResult(Boolean result) {
                if (result != null && result) {
                    mSearchEnginePromoCheckedThisSession = true;
                } else {
                    @SearchEnginePromoType
                    int promoType = getSearchEnginePromoShowType();
                    if (promoType == SEARCH_ENGINE_PROMO_SHOW_EXISTING
                            || promoType == SEARCH_ENGINE_PROMO_SHOW_NEW) {
                        onUserLeavePromoDialogWithNoConfirmedChoice(promoType);
                    }
                }
                if (onSearchEngineFinalized != null) onSearchEngineFinalized.onResult(result);
            }
        };
        if (TemplateUrlService.getInstance().isDefaultSearchManaged()
                || ApiCompatibilityUtils.isDemoUser(activity)) {
            finalizeInternalCallback.onResult(true);
            return;
        }

        final int shouldShow = getSearchEnginePromoShowType();
        Callable<PromoDialog> dialogCreator;
        switch (shouldShow) {
            case SEARCH_ENGINE_PROMO_DONT_SHOW:
                finalizeInternalCallback.onResult(true);
                return;
            case SEARCH_ENGINE_PROMO_SHOW_SOGOU:
                dialogCreator = new Callable<PromoDialog>() {
                    @Override
                    public PromoDialog call() throws Exception {
                        return new SogouPromoDialog(
                                activity, LocaleManager.this, finalizeInternalCallback);
                    }
                };
                break;
            case SEARCH_ENGINE_PROMO_SHOW_EXISTING:
            case SEARCH_ENGINE_PROMO_SHOW_NEW:
                dialogCreator = new Callable<PromoDialog>() {
                    @Override
                    public PromoDialog call() throws Exception {
                        return new DefaultSearchEnginePromoDialog(
                                activity, shouldShow, finalizeInternalCallback);
                    }
                };
                break;
            default:
                assert false;
                finalizeInternalCallback.onResult(true);
                return;
        }

        // If the activity has been destroyed by the time the TemplateUrlService has
        // loaded, then do not attempt to show the dialog.
        if (ApplicationStatus.getStateForActivity(activity) == ActivityState.DESTROYED) {
            finalizeInternalCallback.onResult(false);
            return;
        }

        if (VrIntentUtils.isLaunchingIntoVr(activity)) {
            showPromoDialogForVr(dialogCreator, activity);
        } else {
            showPromoDialog(dialogCreator);
        }
        mSearchEnginePromoShownThisSession = true;
    }

    private void showPromoDialogForVr(Callable<PromoDialog> dialogCreator, Activity activity) {
        VrShellDelegate.requestToExitVrForSearchEnginePromoDialog(new OnExitVrRequestListener() {
            @Override
            public void onSucceeded() {
                showPromoDialog(dialogCreator);
            }

            @Override
            public void onDenied() {
                // We need to make sure that the dialog shows up even if user denied to
                // leave VR.
                VrShellDelegate.forceExitVrImmediately();
                showPromoDialog(dialogCreator);
            }
        }, activity);
    }

    private void showPromoDialog(Callable<PromoDialog> dialogCreator) {
        try {
            dialogCreator.call().show();
        } catch (Exception e) {
            // Exception is caught purely because Callable states it can be thrown.  This is never
            // expected to be hit.
            throw new RuntimeException(e);
        }
    }

    /**
     * @return Whether auto switch for search engine is enabled.
     */
    public boolean isSearchEngineAutoSwitchEnabled() {
        return ContextUtils.getAppSharedPreferences().getBoolean(PREF_AUTO_SWITCH, false);
    }

    /**
     * Sets whether auto switch for search engine is enabled.
     */
    public void setSearchEngineAutoSwitch(boolean isEnabled) {
        ContextUtils.getAppSharedPreferences().edit().putBoolean(PREF_AUTO_SWITCH, isEnabled)
                .apply();
    }

    /**
     * Sets the {@link SnackbarManager} used by this instance.
     */
    public void setSnackbarManager(SnackbarManager manager) {
        mSnackbarManager = new WeakReference<SnackbarManager>(manager);
    }

    private void showSnackbar(CharSequence title) {
        SnackbarManager manager = mSnackbarManager.get();
        if (manager == null) return;

        Context context = ContextUtils.getApplicationContext();
        Snackbar snackbar = Snackbar.make(title, mSnackbarController, Snackbar.TYPE_NOTIFICATION,
                Snackbar.UMA_SPECIAL_LOCALE);
        snackbar.setDuration(SNACKBAR_DURATION_MS);
        snackbar.setAction(context.getString(R.string.preferences), null);
        manager.showSnackbar(snackbar);
    }

    /**
     * Does some extra checking about whether the user is in special locale.
     * @param inSpecialLocale Whether the variation service thinks the client is in special locale.
     * @return The result after extra confirmation.
     */
    protected boolean isReallyInSpecialLocale(boolean inSpecialLocale) {
        return inSpecialLocale;
    }

    /**
     * @return Whether and which search engine promo should be shown.
     */
    @SearchEnginePromoType
    public int getSearchEnginePromoShowType() {
        if (!isSpecialLocaleEnabled()) return SEARCH_ENGINE_PROMO_DONT_SHOW;
        SharedPreferences preferences = ContextUtils.getAppSharedPreferences();
        if (preferences.getBoolean(PREF_PROMO_SHOWN, false)) {
            return SEARCH_ENGINE_PROMO_DONT_SHOW;
        }
        return SEARCH_ENGINE_PROMO_SHOW_SOGOU;
    }

    /**
     * @return The referral ID to be passed when searching with Yandex as the DSE.
     */
    @CalledByNative
    protected String getYandexReferralId() {
        return "";
    }

    /**
     * @return The referral ID to be passed when searching with Mail.RU as the DSE.
     */
    @CalledByNative
    protected String getMailRUReferralId() {
        return "";
    }

    /**
     * To be called after the user has made a selection from a search engine promo dialog.
     * @param type The type of search engine promo dialog that was shown.
     * @param keywords The keywords for all search engines listed in the order shown to the user.
     * @param keyword The keyword for the search engine chosen.
     */
    protected void onUserSearchEngineChoiceFromPromoDialog(
            @SearchEnginePromoType int type, List<String> keywords, String keyword) {
        TemplateUrlService.getInstance().setSearchEngine(keyword);
        ContextUtils.getAppSharedPreferences()
                .edit()
                .putInt(KEY_SEARCH_ENGINE_PROMO_SHOW_STATE, SEARCH_ENGINE_PROMO_CHECKED_AND_SHOWN)
                .apply();
        mSearchEnginePromoCompleted = true;
    }

    /**
     * To be called when the search engine promo dialog is dismissed without the user confirming
     * a valid search engine selection.
     */
    protected void onUserLeavePromoDialogWithNoConfirmedChoice(@SearchEnginePromoType int type) {}

    private LocaleTemplateUrlLoader getLocaleTemplateUrlLoader() {
        if (mLocaleTemplateUrlLoader == null) {
            mLocaleTemplateUrlLoader = new LocaleTemplateUrlLoader(getSpecialLocaleId());
        }
        return mLocaleTemplateUrlLoader;
    }

    /**
     * Get the list of search engines that a user may choose between.
     * @param promoType Which search engine list to show.
     * @return List of engines to show.
     */
    public List<TemplateUrl> getSearchEnginesForPromoDialog(@SearchEnginePromoType int promoType) {
        throw new IllegalStateException(
                "Not applicable unless existing or new promos are required");
    }

    /** Set a LocaleManager to be used for testing. */
    @VisibleForTesting
    public static void setInstanceForTest(LocaleManager instance) {
        sInstance = instance;
    }

    /**
     * Record any locale based metrics related with the search widget. Recorded on initialization
     * only.
     * @param widgetPresent Whether there is at least one search widget on home screen.
     */
    public void recordLocaleBasedSearchWidgetMetrics(boolean widgetPresent) {}

    /**
     * @return Whether the search engine promo has been shown and the user selected a valid option
     *         and successfully completed the promo.
     */
    public boolean hasCompletedSearchEnginePromo() {
        return mSearchEnginePromoCompleted;
    }

    /**
     * @return Whether the search engine promo has been shown in this session.
     */
    public boolean hasShownSearchEnginePromoThisSession() {
        return mSearchEnginePromoShownThisSession;
    }

    /**
     * @return Whether we still have to check for whether search engine dialog is necessary.
     */
    public boolean needToCheckForSearchEnginePromo() {
        if (ChromeFeatureList.isInitialized()
                && !ChromeFeatureList.isEnabled(
                           ChromeFeatureList.SEARCH_ENGINE_PROMO_EXISTING_DEVICE)) {
            return false;
        }
        int state = SEARCH_ENGINE_PROMO_SHOULD_CHECK;
        StrictMode.ThreadPolicy oldPolicy = StrictMode.allowThreadDiskReads();
        try {
            state = ContextUtils.getAppSharedPreferences().getInt(
                    KEY_SEARCH_ENGINE_PROMO_SHOW_STATE, SEARCH_ENGINE_PROMO_SHOULD_CHECK);
        } finally {
            StrictMode.setThreadPolicy(oldPolicy);
        }
        return !mSearchEnginePromoCheckedThisSession && state == SEARCH_ENGINE_PROMO_SHOULD_CHECK;
    }

    /**
     * Record any locale based metrics related with search. Recorded per search.
     * @param isFromSearchWidget Whether the search was performed from the search widget.
     * @param url Url for the search made.
     * @param transition The transition type for the navigation.
     */
    public void recordLocaleBasedSearchMetrics(
            boolean isFromSearchWidget, String url, @PageTransition int transition) {}
}
