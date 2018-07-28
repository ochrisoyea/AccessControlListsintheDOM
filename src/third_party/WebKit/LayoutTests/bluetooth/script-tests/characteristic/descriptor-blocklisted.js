'use strict';
const test_desc = 'Make sure that FUNCTION_NAME can not access blocklisted ' +
    'descriptors.';
const expected = new DOMException(
    'getDescriptor(s) called with blocklisted UUID. https://goo.gl/4NeimX',
    'SecurityError');

// 1. Connect to the device and retrieve the GATT characteristic.
bluetooth_test(() => getBlocklistExcludeReadsCharacteristic()
    // 2. Attempt to call FUNCTION_NAME with a blocklisted UUID.
    .then(({characteristic}) => assert_promise_rejects_with_message(
        // Using UUIDs instead of names to avoid making a name<>uuid mapping.
        characteristic.CALLS([
          getDescriptor('bad2ddcf-60db-45cd-bef9-fd72b153cf7c')|
          getDescriptors('bad2ddcf-60db-45cd-bef9-fd72b153cf7c')
        ]),
        expected,
        'FUNCTION_NAME should reject.')),
    test_desc);