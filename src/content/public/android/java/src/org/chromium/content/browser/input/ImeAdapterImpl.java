// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.content.browser.input;

import android.annotation.SuppressLint;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Rect;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.ResultReceiver;
import android.os.SystemClock;
import android.text.SpannableString;
import android.text.TextUtils;
import android.text.style.BackgroundColorSpan;
import android.text.style.CharacterStyle;
import android.text.style.SuggestionSpan;
import android.text.style.UnderlineSpan;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.ExtractedText;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import org.chromium.base.Log;
import org.chromium.base.TraceEvent;
import org.chromium.base.VisibleForTesting;
import org.chromium.base.annotations.CalledByNative;
import org.chromium.base.annotations.JNINamespace;
import org.chromium.blink_public.web.WebFocusType;
import org.chromium.blink_public.web.WebInputEventModifier;
import org.chromium.blink_public.web.WebInputEventType;
import org.chromium.blink_public.web.WebTextInputMode;
import org.chromium.content.browser.WindowEventObserver;
import org.chromium.content.browser.WindowEventObserverManager;
import org.chromium.content.browser.picker.InputDialogContainer;
import org.chromium.content.browser.webcontents.WebContentsImpl;
import org.chromium.content_public.browser.ImeAdapter;
import org.chromium.content_public.browser.ImeEventObserver;
import org.chromium.content_public.browser.InputMethodManagerWrapper;
import org.chromium.content_public.browser.WebContents;
import org.chromium.content_public.browser.WebContents.UserDataFactory;
import org.chromium.ui.base.ViewAndroidDelegate;
import org.chromium.ui.base.ViewUtils;
import org.chromium.ui.base.ime.TextInputType;

import java.lang.ref.WeakReference;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/**
 * Implementation of the interface {@link ImeAdapter} providing an interface
 * in both ways native <-> java:
 *
 * 1. InputConnectionAdapter notifies native code of text composition state and
 *    dispatch key events from java -> WebKit.
 * 2. Native ImeAdapter notifies java side to clear composition text.
 *
 * The basic flow is:
 * 1. When InputConnectionAdapter gets called with composition or result text:
 *    If we receive a composition text or a result text, then we just need to
 *    dispatch a synthetic key event with special keycode 229, and then dispatch
 *    the composition or result text.
 * 2. Intercept dispatchKeyEvent() method for key events not handled by IME, we
 *   need to dispatch them to webkit and check webkit's reply. Then inject a
 *   new key event for further processing if webkit didn't handle it.
 *
 * Note that the native peer object does not take any strong reference onto the
 * instance of this java object, hence it is up to the client of this class (e.g.
 * the ViewEmbedder implementor) to hold a strong reference to it for the required
 * lifetime of the object.
 */
@JNINamespace("content")
public class ImeAdapterImpl implements ImeAdapter, WindowEventObserver {
    private static final String TAG = "cr_Ime";
    private static final boolean DEBUG_LOGS = false;

    private static final float SUGGESTION_HIGHLIGHT_BACKGROUND_TRANSPARENCY = 0.4f;

    public static final int COMPOSITION_KEY_CODE = ImeAdapter.COMPOSITION_KEY_CODE;
    private static final int IME_FLAG_NO_PERSONALIZED_LEARNING = 0x1000000;

    // Color used by AOSP Android for a SuggestionSpan with FLAG_EASY_CORRECT set
    private static final int DEFAULT_SUGGESTION_SPAN_COLOR = 0x88C8C8C8;

    private long mNativeImeAdapterAndroid;
    private InputMethodManagerWrapper mInputMethodManagerWrapper;
    private ChromiumBaseInputConnection mInputConnection;
    private ChromiumBaseInputConnection.Factory mInputConnectionFactory;

    // NOTE: This object will not be released by Android framework until the matching
    // ResultReceiver in the InputMethodService (IME app) gets gc'ed.
    private ShowKeyboardResultReceiver mShowKeyboardResultReceiver;

    private final WebContentsImpl mWebContents;
    private ViewAndroidDelegate mViewDelegate;

    // This holds the information necessary for constructing CursorAnchorInfo, and notifies to
    // InputMethodManager on appropriate timing, depending on how IME requested the information
    // via InputConnection. The update request is per InputConnection, hence for each time it is
    // re-created, the monitoring status will be reset.
    private CursorAnchorInfoController mCursorAnchorInfoController;

    private final List<ImeEventObserver> mEventObservers = new ArrayList<>();

    private int mTextInputType = TextInputType.NONE;
    private int mTextInputFlags;
    private int mTextInputMode = WebTextInputMode.DEFAULT;
    private boolean mNodeEditable;
    private boolean mNodePassword;

    // Viewport rect before the OSK was brought up.
    // Used to tell View#onSizeChanged to focus a form element.
    private final Rect mFocusPreOSKViewportRect = new Rect();

    // Keep the current configuration to detect the change when onConfigurationChanged() is called.
    private Configuration mCurrentConfig;

    private int mLastSelectionStart;
    private int mLastSelectionEnd;
    private String mLastText;
    private int mLastCompositionStart;
    private int mLastCompositionEnd;
    private boolean mRestartInputOnNextStateUpdate;

    // True if ImeAdapter is connected to render process.
    private boolean mIsConnected;

    /**
     * {@ResultReceiver} passed in InputMethodManager#showSoftInput}. We need this to scroll to the
     * editable node at the right timing, which is after input method window shows up.
     */
    // TODO(crbug.com/635567): Fix this properly.
    @SuppressLint("ParcelCreator")
    private static class ShowKeyboardResultReceiver extends ResultReceiver {
        // Unfortunately, the memory life cycle of ResultReceiver object, once passed in
        // showSoftInput(), is in the control of Android's input method framework and IME app,
        // so we use a weakref to avoid tying ImeAdapter's lifetime to that of ResultReceiver
        // object.
        private final WeakReference<ImeAdapterImpl> mImeAdapter;

        public ShowKeyboardResultReceiver(ImeAdapterImpl imeAdapter, Handler handler) {
            super(handler);
            mImeAdapter = new WeakReference<>(imeAdapter);
        }

        @Override
        public void onReceiveResult(int resultCode, Bundle resultData) {
            ImeAdapterImpl imeAdapter = mImeAdapter.get();
            if (imeAdapter == null) return;
            imeAdapter.onShowKeyboardReceiveResult(resultCode);
        }
    }

    private static final class UserDataFactoryLazyHolder {
        private static final UserDataFactory<ImeAdapterImpl> INSTANCE = ImeAdapterImpl::new;
    }

    /**
     * Create {@link ImeAdapterImpl} instance.
     * @param webContents WebContents instance with which this ImeAdapter is associated.
     * @param wrapper InputMethodManagerWrapper that should receive all the call directed to
     *                InputMethodManager.
     */
    public static ImeAdapterImpl create(
            WebContents webContents, InputMethodManagerWrapper wrapper) {
        ImeAdapterImpl imeAdapter = webContents.getOrSetUserData(
                ImeAdapterImpl.class, UserDataFactoryLazyHolder.INSTANCE);
        assert imeAdapter != null && !imeAdapter.initialized();
        imeAdapter.init(wrapper);
        return imeAdapter;
    }

    private boolean initialized() {
        return mNativeImeAdapterAndroid != 0;
    }

    /**
     * Get {@link ImeAdapter} object used for the give WebContents.
     * {@link #create()} should precede any calls to this.
     * @param webContents {@link WebContents} object.
     * @return {@link ImeAdapter} object. {@code null} if not available because
     *         {@link #create()} is not called yet.
     */
    public static ImeAdapterImpl fromWebContents(WebContents webContents) {
        return webContents.getOrSetUserData(ImeAdapterImpl.class, null);
    }

    /**
     * Returns an instance of the default {@link InputMethodManagerWrapper}
     */
    public static InputMethodManagerWrapper createDefaultInputMethodManagerWrapper(
            Context context) {
        return new InputMethodManagerWrapperImpl(context);
    }

    /**
     * Create {@link ImeAdapterImpl} instance.
     * @param webContents WebContents instance.
     */
    public ImeAdapterImpl(WebContents webContents) {
        mWebContents = (WebContentsImpl) webContents;
        mViewDelegate = mWebContents.getViewAndroidDelegate();
        assert mViewDelegate != null;
    }

    /**
     * @param wrapper InputMethodManagerWrapper that should receive all the call directed to
     *                InputMethodManager.
     */
    private void init(InputMethodManagerWrapper wrapper) {
        mInputMethodManagerWrapper = wrapper;

        // Deep copy newConfig so that we can notice the difference.
        mCurrentConfig = new Configuration(getContainerView().getResources().getConfiguration());

        // CursorAnchroInfo is supported only after L.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            mCursorAnchorInfoController = CursorAnchorInfoController.create(
                    wrapper, new CursorAnchorInfoController.ComposingTextDelegate() {
                        @Override
                        public CharSequence getText() {
                            return mLastText;
                        }
                        @Override
                        public int getSelectionStart() {
                            return mLastSelectionStart;
                        }
                        @Override
                        public int getSelectionEnd() {
                            return mLastSelectionEnd;
                        }
                        @Override
                        public int getComposingTextStart() {
                            return mLastCompositionStart;
                        }
                        @Override
                        public int getComposingTextEnd() {
                            return mLastCompositionEnd;
                        }
                    });
        } else {
            mCursorAnchorInfoController = null;
        }
        mNativeImeAdapterAndroid = nativeInit(mWebContents);
        WindowEventObserverManager.from(mWebContents).addObserver(this);
    }

    @Override
    public InputConnection getActiveInputConnection() {
        return mInputConnection;
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        boolean allowKeyboardLearning = mWebContents != null && !mWebContents.isIncognito();
        return onCreateInputConnection(outAttrs, allowKeyboardLearning);
    }

    @Override
    public boolean onCheckIsTextEditor() {
        return isTextInputType(mTextInputType);
    }

    private boolean isHardwareKeyboardAttached() {
        return mCurrentConfig.keyboard != Configuration.KEYBOARD_NOKEYS;
    }

    @Override
    public void addEventObserver(ImeEventObserver eventObserver) {
        mEventObservers.add(eventObserver);
    }

    private void createInputConnectionFactory() {
        if (mInputConnectionFactory != null) return;
        mInputConnectionFactory = new ThreadedInputConnectionFactory(mInputMethodManagerWrapper);
    }

    // Tells if the ImeAdapter in valid state (i.e. not in destroyed state), and is
    // connected to render process. The former check guards against the call via
    // ThreadedInputConnection from Android framework after ImeAdapter.destroy() is called.
    private boolean isValid() {
        return mNativeImeAdapterAndroid != 0 && mIsConnected;
    }

    // Whether the focused node allows the soft keyboard to be displayed. A content editable
    // region is editable but may disallow the soft keyboard from being displayed. Composition
    // should still be allowed with a physical keyboard so mInputConnection will be non-null.
    private boolean focusedNodeAllowsSoftKeyboard() {
        return mTextInputType != TextInputType.NONE && mTextInputMode != WebTextInputMode.NONE;
    }

    // Whether the focused node is editable or not.
    private boolean focusedNodeEditable() {
        return mTextInputType != TextInputType.NONE;
    }

    private View getContainerView() {
        return mViewDelegate.getContainerView();
    }

    /**
     * @see View#onCreateInputConnection(EditorInfo)
     * @param allowKeyboardLearning Whether to allow keyboard (IME) app to do personalized learning.
     */
    public ChromiumBaseInputConnection onCreateInputConnection(
            EditorInfo outAttrs, boolean allowKeyboardLearning) {
        // InputMethodService evaluates fullscreen mode even when the new input connection is
        // null. This makes sure IME doesn't enter fullscreen mode or open custom UI.
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI;

        // TODO(changwan): Replace with EditorInfoCompat#IME_FLAG_NO_PERSONALIZED_LEARNING or
        //                 EditorInfo#IME_FLAG_NO_PERSONALIZED_LEARNING as soon as either is
        //                 available in all build config types.
        if (!allowKeyboardLearning) outAttrs.imeOptions |= IME_FLAG_NO_PERSONALIZED_LEARNING;
        // Without this line, some third-party IMEs will try to compose text even when
        // not on an editable node. Even when we return null here, key events can still go
        // through ImeAdapter#dispatchKeyEvent().
        if (!focusedNodeEditable()) {
            setInputConnection(null);
            if (DEBUG_LOGS) Log.i(TAG, "onCreateInputConnection returns null.");
            return null;
        }
        if (mInputConnectionFactory == null) return null;
        View containerView = getContainerView();
        setInputConnection(mInputConnectionFactory.initializeAndGet(containerView, this,
                mTextInputType, mTextInputFlags, mTextInputMode, mLastSelectionStart,
                mLastSelectionEnd, outAttrs));
        if (DEBUG_LOGS) Log.i(TAG, "onCreateInputConnection: " + mInputConnection);

        if (mCursorAnchorInfoController != null) {
            mCursorAnchorInfoController.onRequestCursorUpdates(false /* not an immediate request */,
                    false /* disable monitoring */, containerView);
        }
        if (isValid()) {
            nativeRequestCursorUpdate(mNativeImeAdapterAndroid,
                    false /* not an immediate request */, false /* disable monitoring */);
        }
        return mInputConnection;
    }

    private void setInputConnection(ChromiumBaseInputConnection inputConnection) {
        if (mInputConnection == inputConnection) return;
        // The previous input connection might be waiting for state update.
        if (mInputConnection != null) mInputConnection.unblockOnUiThread();
        mInputConnection = inputConnection;
    }

    @Override
    public void setInputMethodManagerWrapper(InputMethodManagerWrapper immw) {
        mInputMethodManagerWrapper = immw;
        if (mCursorAnchorInfoController != null) {
            mCursorAnchorInfoController.setInputMethodManagerWrapper(immw);
        }
    }

    @VisibleForTesting
    void setInputConnectionFactory(ChromiumBaseInputConnection.Factory factory) {
        mInputConnectionFactory = factory;
    }

    @VisibleForTesting
    ChromiumBaseInputConnection.Factory getInputConnectionFactoryForTest() {
        return mInputConnectionFactory;
    }

    @VisibleForTesting
    public void setTriggerDelayedOnCreateInputConnectionForTest(boolean trigger) {
        mInputConnectionFactory.setTriggerDelayedOnCreateInputConnection(trigger);
    }

    /**
     * Get the current input connection for testing purposes.
     */
    @VisibleForTesting
    @Override
    public InputConnection getInputConnectionForTest() {
        return mInputConnection;
    }

    @VisibleForTesting
    @Override
    public void setComposingTextForTest(final CharSequence text, final int newCursorPosition) {
        mInputConnection.getHandler().post(
                () -> mInputConnection.setComposingText(text, newCursorPosition));
    }

    private static int getModifiers(int metaState) {
        int modifiers = 0;
        if ((metaState & KeyEvent.META_SHIFT_ON) != 0) {
            modifiers |= WebInputEventModifier.SHIFT_KEY;
        }
        if ((metaState & KeyEvent.META_ALT_ON) != 0) {
            modifiers |= WebInputEventModifier.ALT_KEY;
        }
        if ((metaState & KeyEvent.META_CTRL_ON) != 0) {
            modifiers |= WebInputEventModifier.CONTROL_KEY;
        }
        if ((metaState & KeyEvent.META_CAPS_LOCK_ON) != 0) {
            modifiers |= WebInputEventModifier.CAPS_LOCK_ON;
        }
        if ((metaState & KeyEvent.META_NUM_LOCK_ON) != 0) {
            modifiers |= WebInputEventModifier.NUM_LOCK_ON;
        }
        return modifiers;
    }

    /**
     * Updates internal representation of the text being edited and its selection and composition
     * properties.
     *
     * @param textInputType Text input type for the currently focused field in renderer.
     * @param textInputFlags Text input flags.
     * @param textInputMode Text input mode.
     * @param showIfNeeded Whether the keyboard should be shown if it is currently hidden.
     * @param text The String contents of the field being edited.
     * @param selectionStart The character offset of the selection start, or the caret position if
     *                       there is no selection.
     * @param selectionEnd The character offset of the selection end, or the caret position if there
     *                     is no selection.
     * @param compositionStart The character offset of the composition start, or -1 if there is no
     *                         composition.
     * @param compositionEnd The character offset of the composition end, or -1 if there is no
     *                       selection.
     * @param replyToRequest True when the update was requested by IME.
     */
    @CalledByNative
    private void updateState(int textInputType, int textInputFlags, int textInputMode,
            boolean showIfNeeded, String text, int selectionStart, int selectionEnd,
            int compositionStart, int compositionEnd, boolean replyToRequest) {
        TraceEvent.begin("ImeAdapter.updateState");
        try {
            if (DEBUG_LOGS) {
                Log.i(TAG, "updateState: type [%d->%d], flags [%d], mode[%d], show [%b], ",
                        mTextInputType, textInputType, textInputFlags, textInputMode, showIfNeeded);
            }
            boolean needsRestart = false;
            boolean hide = false;
            if (mRestartInputOnNextStateUpdate) {
                needsRestart = true;
                mRestartInputOnNextStateUpdate = false;
            }

            mTextInputFlags = textInputFlags;
            if (mTextInputMode != textInputMode) {
                mTextInputMode = textInputMode;
                hide = textInputMode == WebTextInputMode.NONE && !isHardwareKeyboardAttached();
                needsRestart = true;
            }
            if (mTextInputType != textInputType) {
                mTextInputType = textInputType;
                if (textInputType == TextInputType.NONE) {
                    hide = true;
                } else {
                    needsRestart = true;
                }
            }

            boolean editable = focusedNodeEditable();
            boolean password = textInputType == TextInputType.PASSWORD;
            if (mNodeEditable != editable || mNodePassword != password) {
                for (ImeEventObserver observer : mEventObservers) {
                    observer.onNodeAttributeUpdated(editable, password);
                }
                mNodeEditable = editable;
                mNodePassword = password;
            }
            if (mCursorAnchorInfoController != null
                    && (!TextUtils.equals(mLastText, text) || mLastSelectionStart != selectionStart
                               || mLastSelectionEnd != selectionEnd
                               || mLastCompositionStart != compositionStart
                               || mLastCompositionEnd != compositionEnd)) {
                mCursorAnchorInfoController.invalidateLastCursorAnchorInfo();
            }
            mLastText = text;
            mLastSelectionStart = selectionStart;
            mLastSelectionEnd = selectionEnd;
            mLastCompositionStart = compositionStart;
            mLastCompositionEnd = compositionEnd;

            if (hide) {
                hideKeyboard();
            } else {
                if (needsRestart) restartInput();
                if (showIfNeeded && focusedNodeAllowsSoftKeyboard()) {
                    // There is no API for us to get notified of user's dismissal of keyboard.
                    // Therefore, we should try to show keyboard even when text input type hasn't
                    // changed.
                    showSoftKeyboard();
                }
            }

            if (mInputConnection != null) {
                boolean singleLine = mTextInputType != TextInputType.TEXT_AREA
                        && mTextInputType != TextInputType.CONTENT_EDITABLE;
                mInputConnection.updateStateOnUiThread(text, selectionStart, selectionEnd,
                        compositionStart, compositionEnd, singleLine, replyToRequest);
            }
        } finally {
            TraceEvent.end("ImeAdapter.updateState");
        }
    }

    /**
     * Show soft keyboard only if it is the current keyboard configuration.
     */
    private void showSoftKeyboard() {
        if (!isValid()) return;
        if (DEBUG_LOGS) Log.i(TAG, "showSoftKeyboard");
        View containerView = getContainerView();
        mInputMethodManagerWrapper.showSoftInput(containerView, 0, getNewShowKeyboardReceiver());
        if (containerView.getResources().getConfiguration().keyboard
                != Configuration.KEYBOARD_NOKEYS) {
            mWebContents.scrollFocusedEditableNodeIntoView();
        }
    }

    /**
     * Call this when we get result from ResultReceiver passed in calling showSoftInput().
     * @param resultCode The result of showSoftInput() as defined in InputMethodManager.
     */
    public void onShowKeyboardReceiveResult(int resultCode) {
        View containerView = getContainerView();
        if (resultCode == InputMethodManager.RESULT_SHOWN) {
            // If OSK is newly shown, delay the form focus until
            // the onSizeChanged (in order to adjust relative to the
            // new size).
            // TODO(jdduke): We should not assume that onSizeChanged will
            // always be called, crbug.com/294908.
            containerView.getWindowVisibleDisplayFrame(mFocusPreOSKViewportRect);
        } else if (ViewUtils.hasFocus(containerView)
                && resultCode == InputMethodManager.RESULT_UNCHANGED_SHOWN) {
            // If the OSK was already there, focus the form immediately.
            mWebContents.scrollFocusedEditableNodeIntoView();
        }
    }

    @CalledByNative
    private void updateAfterViewSizeChanged() {
        // Execute a delayed form focus operation because the OSK was brought up earlier.
        if (!mFocusPreOSKViewportRect.isEmpty()) {
            Rect rect = new Rect();
            getContainerView().getWindowVisibleDisplayFrame(rect);
            if (!rect.equals(mFocusPreOSKViewportRect)) {
                // Only assume the OSK triggered the onSizeChanged if width was preserved.
                if (rect.width() == mFocusPreOSKViewportRect.width()) {
                    assert mWebContents != null;
                    mWebContents.scrollFocusedEditableNodeIntoView();
                }
                // Zero the rect to prevent the above operation from issuing the delayed
                // form focus event.
                cancelRequestToScrollFocusedEditableNodeIntoView();
            }
        }
    }

    @CalledByNative
    private void updateOnTouchDown() {
        cancelRequestToScrollFocusedEditableNodeIntoView();
    }

    public void cancelRequestToScrollFocusedEditableNodeIntoView() {
        mFocusPreOSKViewportRect.setEmpty();
    }

    @Override
    public ResultReceiver getNewShowKeyboardReceiver() {
        if (mShowKeyboardResultReceiver == null) {
            // Note: the returned object will get leaked by Android framework.
            mShowKeyboardResultReceiver = new ShowKeyboardResultReceiver(this, new Handler());
        }
        return mShowKeyboardResultReceiver;
    }

    /**
     * Hide soft keyboard.
     */
    private void hideKeyboard() {
        if (!isValid()) return;
        if (DEBUG_LOGS) Log.i(TAG, "hideKeyboard");
        View view = mViewDelegate.getContainerView();
        if (mInputMethodManagerWrapper.isActive(view)) {
            // NOTE: we should not set ResultReceiver here. Otherwise, IMM will own ContentViewCore
            // and ImeAdapter even after input method goes away and result gets received.
            mInputMethodManagerWrapper.hideSoftInputFromWindow(view.getWindowToken(), 0, null);
        }
        // Detach input connection by returning null from onCreateInputConnection().
        if (!focusedNodeEditable() && mInputConnection != null) {
            ChromiumBaseInputConnection inputConnection = mInputConnection;
            restartInput(); // resets mInputConnection
            // crbug.com/666982: Restart input may not happen if view is detached from window, but
            // we need to unblock in any case. We want to call this after restartInput() to
            // ensure that there is no additional IME operation in the queue.
            inputConnection.unblockOnUiThread();
        }
    }

    /**
     * Call this when keyboard configuration has changed.
     */
    public void onKeyboardConfigurationChanged(Configuration newConfig) {
        if (!isValid()) return;
        // If configuration unchanged, do nothing.
        if (mCurrentConfig.keyboard == newConfig.keyboard
                && mCurrentConfig.keyboardHidden == newConfig.keyboardHidden
                && mCurrentConfig.hardKeyboardHidden == newConfig.hardKeyboardHidden) {
            return;
        }

        // Deep copy newConfig so that we can notice the difference.
        mCurrentConfig = new Configuration(newConfig);
        if (DEBUG_LOGS) {
            Log.i(TAG, "onKeyboardConfigurationChanged: mTextInputType [%d]", mTextInputType);
        }
        if (focusedNodeAllowsSoftKeyboard()) {
            restartInput();
            // By default, we show soft keyboard on keyboard changes. This is useful
            // when the user switches from hardware keyboard to software keyboard.
            // TODO(changwan): check if we can skip this for hardware keyboard configurations.
            showSoftKeyboard();
        } else if (focusedNodeEditable()) {
            // The focused node is editable but disllows the virtual keyboard. We may need to
            // show soft keyboard (for IME composition window only) if a hardware keyboard is
            // present.
            restartInput();
            if (!isHardwareKeyboardAttached())
                hideKeyboard();
            else
                showSoftKeyboard();
        }
    }

    // WindowEventObserver

    @Override
    public void onWindowFocusChanged(boolean gainFocus) {
        if (mInputConnectionFactory != null) {
            mInputConnectionFactory.onWindowFocusChanged(gainFocus);
        }
    }

    @Override
    public void onAttachedToWindow() {
        if (mInputConnectionFactory != null) {
            mInputConnectionFactory.onViewAttachedToWindow();
        }
    }

    @Override
    public void onDetachedFromWindow() {
        resetAndHideKeyboard();
        if (mInputConnectionFactory != null) {
            mInputConnectionFactory.onViewDetachedFromWindow();
        }
    }

    /**
     * Call this when view's focus has changed.
     * @param gainFocus True if we're gaining focus.
     * @param hideKeyboardOnBlur True if we should hide soft keyboard when losing focus.
     */
    public void onViewFocusChanged(boolean gainFocus, boolean hideKeyboardOnBlur) {
        if (DEBUG_LOGS) Log.i(TAG, "onViewFocusChanged: gainFocus [%b]", gainFocus);
        if (!gainFocus && hideKeyboardOnBlur) resetAndHideKeyboard();
        if (mInputConnectionFactory != null) {
            mInputConnectionFactory.onViewFocusChanged(gainFocus);
        }
    }

    private static boolean isTextInputType(int type) {
        return type != TextInputType.NONE && !InputDialogContainer.isDialogInputType(type);
    }

    /**
     * See {@link View#dispatchKeyEvent(KeyEvent)}
     */
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (DEBUG_LOGS) {
            Log.i(TAG, "dispatchKeyEvent: action [%d], keycode [%d]", event.getAction(),
                    event.getKeyCode());
        }
        if (mInputConnection != null) return mInputConnection.sendKeyEventOnUiThread(event);
        return sendKeyEvent(event);
    }

    /**
     * Resets IME adapter and hides keyboard. Note that this will also unblock input connection.
     */
    public void resetAndHideKeyboard() {
        if (DEBUG_LOGS) Log.i(TAG, "resetAndHideKeyboard");
        mTextInputType = TextInputType.NONE;
        mTextInputFlags = 0;
        mTextInputMode = WebTextInputMode.DEFAULT;
        mRestartInputOnNextStateUpdate = false;
        // This will trigger unblocking if necessary.
        hideKeyboard();
    }

    public void reset() {
        resetAndHideKeyboard();
    }

    @CalledByNative
    private void destroy() {
        resetAndHideKeyboard();
        mNativeImeAdapterAndroid = 0;
        mIsConnected = false;
        if (mCursorAnchorInfoController != null) {
            mCursorAnchorInfoController.focusedNodeChanged(false);
        }
    }

    /**
     * Update selection to input method manager.
     *
     * @param selectionStart   The selection start.
     * @param selectionEnd     The selection end.
     * @param compositionStart The composition start.
     * @param compositionEnd   The composition end.
     */
    void updateSelection(
            int selectionStart, int selectionEnd, int compositionStart, int compositionEnd) {
        mInputMethodManagerWrapper.updateSelection(
                getContainerView(), selectionStart, selectionEnd, compositionStart, compositionEnd);
    }

    /**
     * Update extracted text to input method manager.
     */
    void updateExtractedText(int token, ExtractedText extractedText) {
        mInputMethodManagerWrapper.updateExtractedText(getContainerView(), token, extractedText);
    }

    /**
     * Restart input (finish composition and change EditorInfo, such as input type).
     */
    void restartInput() {
        if (!isValid()) return;
        // This will eventually cause input method manager to call View#onCreateInputConnection().
        mInputMethodManagerWrapper.restartInput(getContainerView());
        if (mInputConnection != null) mInputConnection.onRestartInputOnUiThread();
    }

    /**
     * @see BaseInputConnection#performContextMenuAction(int)
     */
    boolean performContextMenuAction(int id) {
        if (DEBUG_LOGS) Log.i(TAG, "performContextMenuAction: id [%d]", id);
        switch (id) {
            case android.R.id.selectAll:
                mWebContents.selectAll();
                return true;
            case android.R.id.cut:
                mWebContents.cut();
                return true;
            case android.R.id.copy:
                mWebContents.copy();
                return true;
            case android.R.id.paste:
                mWebContents.paste();
                return true;
            default:
                return false;
        }
    }

    boolean performEditorAction(int actionCode) {
        if (!isValid()) return false;
        switch (actionCode) {
            case EditorInfo.IME_ACTION_NEXT:
                advanceFocusInForm(WebFocusType.FORWARD);
                break;
            case EditorInfo.IME_ACTION_PREVIOUS:
                advanceFocusInForm(WebFocusType.BACKWARD);
                break;
            default:
                sendSyntheticKeyPress(KeyEvent.KEYCODE_ENTER,
                        KeyEvent.FLAG_SOFT_KEYBOARD | KeyEvent.FLAG_KEEP_TOUCH_MODE
                                | KeyEvent.FLAG_EDITOR_ACTION);
                break;
        }
        return true;
    }

    /**
     * Advances the focus to next input field in the current form.
     *
     * @param focusType indicates whether to advance forward or backward direction.
     */
    private void advanceFocusInForm(int focusType) {
        if (mNativeImeAdapterAndroid == 0) return;
        nativeAdvanceFocusInForm(mNativeImeAdapterAndroid, focusType);
    }

    void notifyUserAction() {
        mInputMethodManagerWrapper.notifyUserAction();
    }

    public void sendSyntheticKeyPress(int keyCode, int flags) {
        long eventTime = SystemClock.uptimeMillis();
        sendKeyEvent(new KeyEvent(eventTime, eventTime, KeyEvent.ACTION_DOWN, keyCode, 0, 0,
                KeyCharacterMap.VIRTUAL_KEYBOARD, 0, flags));
        sendKeyEvent(new KeyEvent(eventTime, eventTime, KeyEvent.ACTION_UP, keyCode, 0, 0,
                KeyCharacterMap.VIRTUAL_KEYBOARD, 0, flags));
    }

    private void onImeEvent() {
        for (ImeEventObserver observer : mEventObservers) observer.onImeEvent();
        if (mNodeEditable) mWebContents.dismissTextHandles();
    }

    boolean sendCompositionToNative(
            CharSequence text, int newCursorPosition, boolean isCommit, int unicodeFromKeyEvent) {
        if (!isValid()) return false;

        onImeEvent();
        long timestampMs = SystemClock.uptimeMillis();
        nativeSendKeyEvent(mNativeImeAdapterAndroid, null, WebInputEventType.RAW_KEY_DOWN, 0,
                timestampMs, COMPOSITION_KEY_CODE, 0, false, unicodeFromKeyEvent);

        if (isCommit) {
            nativeCommitText(mNativeImeAdapterAndroid, text, text.toString(), newCursorPosition);
        } else {
            nativeSetComposingText(
                    mNativeImeAdapterAndroid, text, text.toString(), newCursorPosition);
        }

        nativeSendKeyEvent(mNativeImeAdapterAndroid, null, WebInputEventType.KEY_UP, 0, timestampMs,
                COMPOSITION_KEY_CODE, 0, false, unicodeFromKeyEvent);
        return true;
    }

    @VisibleForTesting
    boolean finishComposingText() {
        if (!isValid()) return false;
        nativeFinishComposingText(mNativeImeAdapterAndroid);
        return true;
    }

    boolean sendKeyEvent(KeyEvent event) {
        if (!isValid()) return false;

        int action = event.getAction();
        int type;
        if (action == KeyEvent.ACTION_DOWN) {
            type = WebInputEventType.KEY_DOWN;
        } else if (action == KeyEvent.ACTION_UP) {
            type = WebInputEventType.KEY_UP;
        } else {
            // In theory, KeyEvent.ACTION_MULTIPLE is a valid value, but in practice
            // this seems to have been quietly deprecated and we've never observed
            // a case where it's sent (holding down physical keyboard key also
            // sends ACTION_DOWN), so it's fine to silently drop it.
            return false;
        }

        for (ImeEventObserver observer : mEventObservers) observer.onBeforeSendKeyEvent(event);
        onImeEvent();

        return nativeSendKeyEvent(mNativeImeAdapterAndroid, event, type,
                getModifiers(event.getMetaState()), event.getEventTime(), event.getKeyCode(),
                event.getScanCode(), /*isSystemKey=*/false, event.getUnicodeChar());
    }

    /**
     * Send a request to the native counterpart to delete a given range of characters.
     * @param beforeLength Number of characters to extend the selection by before the existing
     *                     selection.
     * @param afterLength Number of characters to extend the selection by after the existing
     *                    selection.
     * @return Whether the native counterpart of ImeAdapter received the call.
     */
    boolean deleteSurroundingText(int beforeLength, int afterLength) {
        onImeEvent();
        if (!isValid()) return false;
        nativeSendKeyEvent(mNativeImeAdapterAndroid, null, WebInputEventType.RAW_KEY_DOWN, 0,
                SystemClock.uptimeMillis(), COMPOSITION_KEY_CODE, 0, false, 0);
        nativeDeleteSurroundingText(mNativeImeAdapterAndroid, beforeLength, afterLength);
        nativeSendKeyEvent(mNativeImeAdapterAndroid, null, WebInputEventType.KEY_UP, 0,
                SystemClock.uptimeMillis(), COMPOSITION_KEY_CODE, 0, false, 0);
        return true;
    }

    /**
     * Send a request to the native counterpart to delete a given range of characters.
     * @param beforeLength Number of code points to extend the selection by before the existing
     *                     selection.
     * @param afterLength Number of code points to extend the selection by after the existing
     *                    selection.
     * @return Whether the native counterpart of ImeAdapter received the call.
     */
    boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {
        onImeEvent();
        if (!isValid()) return false;
        nativeSendKeyEvent(mNativeImeAdapterAndroid, null, WebInputEventType.RAW_KEY_DOWN, 0,
                SystemClock.uptimeMillis(), COMPOSITION_KEY_CODE, 0, false, 0);
        nativeDeleteSurroundingTextInCodePoints(
                mNativeImeAdapterAndroid, beforeLength, afterLength);
        nativeSendKeyEvent(mNativeImeAdapterAndroid, null, WebInputEventType.KEY_UP, 0,
                SystemClock.uptimeMillis(), COMPOSITION_KEY_CODE, 0, false, 0);
        return true;
    }

    /**
     * Send a request to the native counterpart to set the selection to given range.
     * @param start Selection start index.
     * @param end Selection end index.
     * @return Whether the native counterpart of ImeAdapter received the call.
     */
    boolean setEditableSelectionOffsets(int start, int end) {
        if (!isValid()) return false;
        nativeSetEditableSelectionOffsets(mNativeImeAdapterAndroid, start, end);
        return true;
    }

    /**
     * Send a request to the native counterpart to set composing region to given indices.
     * @param start The start of the composition.
     * @param end The end of the composition.
     * @return Whether the native counterpart of ImeAdapter received the call.
     */
    boolean setComposingRegion(int start, int end) {
        if (!isValid()) return false;
        if (start <= end) {
            nativeSetComposingRegion(mNativeImeAdapterAndroid, start, end);
        } else {
            nativeSetComposingRegion(mNativeImeAdapterAndroid, end, start);
        }
        return true;
    }

    @CalledByNative
    private void focusedNodeChanged(boolean isEditable) {
        if (DEBUG_LOGS) Log.i(TAG, "focusedNodeChanged: isEditable [%b]", isEditable);

        // Update controller before the connection is restarted.
        if (mCursorAnchorInfoController != null) {
            mCursorAnchorInfoController.focusedNodeChanged(isEditable);
        }

        if (mTextInputType != TextInputType.NONE && mInputConnection != null && isEditable) {
            mRestartInputOnNextStateUpdate = true;
        }
    }

    /**
     * Send a request to the native counterpart to give the latest text input state update.
     */
    boolean requestTextInputStateUpdate() {
        if (!isValid()) return false;
        // You won't get state update anyways.
        if (mInputConnection == null) return false;
        return nativeRequestTextInputStateUpdate(mNativeImeAdapterAndroid);
    }

    /**
     * Notified when IME requested Chrome to change the cursor update mode.
     */
    public boolean onRequestCursorUpdates(int cursorUpdateMode) {
        final boolean immediateRequest =
                (cursorUpdateMode & InputConnection.CURSOR_UPDATE_IMMEDIATE) != 0;
        final boolean monitorRequest =
                (cursorUpdateMode & InputConnection.CURSOR_UPDATE_MONITOR) != 0;

        if (isValid()) {
            nativeRequestCursorUpdate(mNativeImeAdapterAndroid, immediateRequest, monitorRequest);
        }
        if (mCursorAnchorInfoController == null) return false;
        return mCursorAnchorInfoController.onRequestCursorUpdates(
                immediateRequest, monitorRequest, getContainerView());
    }

    /**
     * Notified when a frame has been produced by the renderer and all the associated metadata.
     * @param scaleFactor device scale factor.
     * @param contentOffsetYPix Y offset below the browser controls.
     * @param hasInsertionMarker Whether the insertion marker is visible or not.
     * @param insertionMarkerHorizontal X coordinates (in view-local DIP pixels) of the insertion
     *                                  marker if it exists. Will be ignored otherwise.
     * @param insertionMarkerTop Y coordinates (in view-local DIP pixels) of the top of the
     *                           insertion marker if it exists. Will be ignored otherwise.
     * @param insertionMarkerBottom Y coordinates (in view-local DIP pixels) of the bottom of
     *                              the insertion marker if it exists. Will be ignored otherwise.
     */
    @CalledByNative
    private void updateFrameInfo(float scaleFactor, float contentOffsetYPix,
            boolean hasInsertionMarker, boolean isInsertionMarkerVisible,
            float insertionMarkerHorizontal, float insertionMarkerTop,
            float insertionMarkerBottom) {
        if (mCursorAnchorInfoController == null) return;
        mCursorAnchorInfoController.onUpdateFrameInfo(scaleFactor, contentOffsetYPix,
                hasInsertionMarker, isInsertionMarkerVisible, insertionMarkerHorizontal,
                insertionMarkerTop, insertionMarkerBottom, getContainerView());
    }

    private int getUnderlineColorForSuggestionSpan(SuggestionSpan suggestionSpan) {
        try {
            Method getUnderlineColorMethod = SuggestionSpan.class.getMethod("getUnderlineColor");
            return (int) getUnderlineColorMethod.invoke(suggestionSpan);
        } catch (IllegalAccessException e) {
            return DEFAULT_SUGGESTION_SPAN_COLOR;
        } catch (InvocationTargetException e) {
            return DEFAULT_SUGGESTION_SPAN_COLOR;
        } catch (NoSuchMethodException e) {
            return DEFAULT_SUGGESTION_SPAN_COLOR;
        }
    }

    @CalledByNative
    private void populateImeTextSpansFromJava(CharSequence text, long imeTextSpans) {
        if (DEBUG_LOGS) {
            Log.i(TAG, "populateImeTextSpansFromJava: text [%s], ime_text_spans [%d]", text,
                    imeTextSpans);
        }
        if (!(text instanceof SpannableString)) return;

        SpannableString spannableString = ((SpannableString) text);
        CharacterStyle spans[] = spannableString.getSpans(0, text.length(), CharacterStyle.class);
        for (CharacterStyle span : spans) {
            if (span instanceof BackgroundColorSpan) {
                nativeAppendBackgroundColorSpan(imeTextSpans, spannableString.getSpanStart(span),
                        spannableString.getSpanEnd(span),
                        ((BackgroundColorSpan) span).getBackgroundColor());
            } else if (span instanceof UnderlineSpan) {
                nativeAppendUnderlineSpan(imeTextSpans, spannableString.getSpanStart(span),
                        spannableString.getSpanEnd(span));
            } else if (span instanceof SuggestionSpan) {
                final SuggestionSpan suggestionSpan = (SuggestionSpan) span;

                // We currently only support FLAG_EASY_CORRECT and FLAG_MISSPELLED SuggestionSpans.

                // Other types:
                // - FLAG_AUTO_CORRECTION is used e.g. by Samsung's IME to flash a blue background
                //   on a word being replaced by an autocorrect suggestion. We don't currently
                //   support this.
                //
                // - Some IMEs (e.g. the AOSP keyboard on Jelly Bean) add SuggestionSpans with no
                //   flags set and no underline color to add suggestions to words marked as
                //   misspelled (instead of having the spell checker return the suggestions when
                //   called). We don't support these either.
                final boolean isMisspellingSpan =
                        (suggestionSpan.getFlags() & SuggestionSpan.FLAG_MISSPELLED) != 0;
                if (suggestionSpan.getFlags() != SuggestionSpan.FLAG_EASY_CORRECT
                        && !isMisspellingSpan) {
                    continue;
                }

                // Copied from Android's Editor.java so we use the same colors
                // as the native Android text widget.
                final int underlineColor = getUnderlineColorForSuggestionSpan(suggestionSpan);
                final int newAlpha = (int) (Color.alpha(underlineColor)
                        * SUGGESTION_HIGHLIGHT_BACKGROUND_TRANSPARENCY);
                final int suggestionHighlightColor =
                        (underlineColor & 0x00FFFFFF) + (newAlpha << 24);

                nativeAppendSuggestionSpan(imeTextSpans,
                        spannableString.getSpanStart(suggestionSpan),
                        spannableString.getSpanEnd(suggestionSpan), isMisspellingSpan,
                        underlineColor, suggestionHighlightColor, suggestionSpan.getSuggestions());
            }
        }
    }

    @CalledByNative
    private void cancelComposition() {
        if (DEBUG_LOGS) Log.i(TAG, "cancelComposition");
        if (mInputConnection != null) restartInput();
    }

    @CalledByNative
    private void setCharacterBounds(float[] characterBounds) {
        if (mCursorAnchorInfoController == null) return;
        mCursorAnchorInfoController.setCompositionCharacterBounds(
                characterBounds, getContainerView());
    }

    @CalledByNative
    private void onConnectedToRenderProcess() {
        if (DEBUG_LOGS) Log.i(TAG, "onConnectedToRenderProcess");
        mIsConnected = true;
        createInputConnectionFactory();
        resetAndHideKeyboard();
    }

    private native long nativeInit(WebContents webContents);
    private native boolean nativeSendKeyEvent(long nativeImeAdapterAndroid, KeyEvent event,
            int type, int modifiers, long timestampMs, int keyCode, int scanCode,
            boolean isSystemKey, int unicodeChar);
    private static native void nativeAppendUnderlineSpan(long spanPtr, int start, int end);
    private static native void nativeAppendBackgroundColorSpan(
            long spanPtr, int start, int end, int backgroundColor);
    private static native void nativeAppendSuggestionSpan(long spanPtr, int start, int end,
            boolean isMisspelling, int underlineColor, int suggestionHighlightColor,
            String[] suggestions);
    private native void nativeSetComposingText(
            long nativeImeAdapterAndroid, CharSequence text, String textStr, int newCursorPosition);
    private native void nativeCommitText(
            long nativeImeAdapterAndroid, CharSequence text, String textStr, int newCursorPosition);
    private native void nativeFinishComposingText(long nativeImeAdapterAndroid);
    private native void nativeSetEditableSelectionOffsets(
            long nativeImeAdapterAndroid, int start, int end);
    private native void nativeSetComposingRegion(long nativeImeAdapterAndroid, int start, int end);
    private native void nativeDeleteSurroundingText(
            long nativeImeAdapterAndroid, int before, int after);
    private native void nativeDeleteSurroundingTextInCodePoints(
            long nativeImeAdapterAndroid, int before, int after);
    private native boolean nativeRequestTextInputStateUpdate(long nativeImeAdapterAndroid);
    private native void nativeRequestCursorUpdate(
            long nativeImeAdapterAndroid, boolean immediateRequest, boolean monitorRequest);
    private native void nativeAdvanceFocusInForm(long nativeImeAdapterAndroid, int focusType);
}
