// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

Polymer({
  is: 'print-preview-scaling-settings',

  behaviors: [SettingsBehavior],

  properties: {
    /** @type {Object} */
    documentInfo: Object,

    /** @private {string} */
    currentValue_: String,

    /** @private {boolean} */
    inputValid_: Boolean,

    /** @private {boolean} */
    hideInput_: Boolean,

    disabled: Boolean,
  },

  /** @private {string} */
  lastValidScaling_: '100',

  /** @private {number} */
  fitToPageFlag_: 0,

  observers: [
    'onFitToPageSettingChange_(settings.fitToPage.value, ' +
        'settings.fitToPage.available, documentInfo.fitToPageScaling)',
    'onInputChanged_(currentValue_, inputValid_)',
    'onScalingSettingChanged_(settings.scaling.value)',
  ],

  /** @private */
  onFitToPageSettingChange_: function() {
    const fitToPage = this.getSetting('fitToPage');
    if (!fitToPage.available)
      return;
    this.$$('#fit-to-page-checkbox').checked = fitToPage.value;
    if (!fitToPage.value) {
      // Fit to page is no longer checked. Update the display.
      this.currentValue_ = this.lastValidScaling_;
    } else if (fitToPage.value) {
      // Set flag to number of expected calls to onInputChanged_. If scaling
      // is valid, 1 call will occur due to the change to |currentValue_|. If
      // not, 2 calls will occur, since |inputValid_| will also change.
      this.fitToPageFlag_ = this.inputValid_ ? 1 : 2;
      this.currentValue_ = this.documentInfo.fitToPageScaling.toString();
    }
  },

  /**
   * Updates the input string when scaling setting is set.
   * @private
   */
  onScalingSettingChanged_: function() {
    // Update last valid scaling and ensure input string matches.
    this.lastValidScaling_ =
        /** @type {string} */ (this.getSetting('scaling').value);
    this.currentValue_ = this.lastValidScaling_;
  },

  /**
   * Updates scaling and fit to page settings based on the validity and current
   * value of the scaling input.
   * @private
   */
  onInputChanged_: function() {
    const fitToPage = this.$$('#fit-to-page-checkbox').checked;
    if (fitToPage && this.fitToPageFlag_ == 0) {
      // User modified scaling while fit to page was checked. Uncheck fit to
      // page.
      const wasValid = this.getSetting('scaling').valid;
      if (this.inputValid_ && wasValid)
        this.setSetting('scaling', this.currentValue_);
      else
        this.setSettingValid('scaling', false);
      this.$$('#fit-to-page-checkbox').checked = false;
      this.setSetting('fitToPage', false);
    } else if (fitToPage) {
      // Fit to page was checked and scaling changed as a result.
      this.fitToPageFlag_--;
      this.setSettingValid('scaling', true);
    } else {
      // User modified scaling while fit to page was not checked or
      // scaling setting was set.
      const wasValid = this.getSetting('scaling').valid;
      this.setSettingValid('scaling', this.inputValid_);
      if (this.inputValid_ && wasValid)
        this.setSetting('scaling', this.currentValue_);
    }
  },

  /** @private */
  onFitToPageChange_: function() {
    this.setSetting('fitToPage', this.$$('#fit-to-page-checkbox').checked);
  },

  /**
   * @return {boolean} Whether the input should be disabled.
   * @private
   */
  getDisabled_: function() {
    return this.disabled && this.inputValid_;
  },
});
