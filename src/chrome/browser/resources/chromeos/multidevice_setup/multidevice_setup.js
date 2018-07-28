// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

cr.define('multidevice_setup', function() {
  /** @enum {string} */
  const PageName = {
    FAILURE: 'setup-failed-page',
    SUCCESS: 'setup-succeeded-page',
    START: 'start-setup-page',
  };

  const MultiDeviceSetup = Polymer({
    is: 'multidevice-setup',

    properties: {
      /**
       * Element name of the currently visible page.
       *
       * @private {!PageName}
       */
      visiblePageName_: {
        type: String,
        value: PageName.START,
      },

      /**
       * DOM Element corresponding to the visible page.
       *
       * @private {!StartSetupPageElement|!SetupSucceededPageElement|
       *           !SetupFailedPageElement}
       */
      visiblePage_: Object,

      /**
       * Array of objects representing all available MultiDevice hosts. Each
       * object contains the name of the device type (e.g. "Pixel XL") and its
       * Device ID.
       *
       * @private {Array<{name: string, id: string}>}
       */
      devices_: Array,

      /**
       * Device ID for the currently selected host device.
       *
       * Undefined if the no list of potential hosts has been received from mojo
       * service.
       *
       * @private {string|undefined}
       */
      selectedDeviceId_: String,
    },

    listeners: {
      'forward-navigation-requested': 'onForwardNavigationRequested_',
      'backward-navigation-requested': 'onBackwardNavigationRequested_',
    },

    // Event handling callbacks

    /** @private */
    onForwardNavigationRequested_: function() {
      switch (this.visiblePageName_) {
        case PageName.FAILURE:
          this.visiblePageName_ = PageName.START;
          break;
        case PageName.SUCCESS:
          this.closeUi_();
          break;
        case PageName.START:
          // TODO(jordynass): Once mojo API is complete, this should call
          // SetBetterTogetherHost(selectedDeviceId)
          console.log(
              'Calling SetBetterTogetherHost on device ',
              this.selectedDeviceId_);
      }
    },

    /** @private */
    onBackwardNavigationRequested_: function() {
      switch (this.visiblePageName_) {
        case PageName.FAILURE:
          this.closeUi_();
          break;
        case PageName.START:
          this.closeUi_();
      }
    },

    // Instance methods

    /** @private */
    closeUi_: function() {
      // TODO(jordynass): Implement closing UI.
      console.log('Closing WebUI');
      // This method is just for testing that the method was called
      this.fire('ui-closed');
    },

    /**
     * @param {number} deviceCount
     * @private
     */
    // TODO(jordynass): Remove dummy testing methods getFakeDevices_
    getFakeDevices_: function(deviceCount) {
      const deviceNames = ['Pixel', 'Pixel XL', 'Nexus 5', 'Nexus 6P'];
      let newDevices = [];
      for (let i = 0; i < deviceCount; i++) {
        const deviceName = deviceNames[i % 4];
        newDevices.push({name: deviceName, id: deviceName + '--' + i});
      }
      this.devices_ = newDevices;
    }
  });

  return {
    MultiDeviceSetup: MultiDeviceSetup,
  };
});