// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_COMPONENTS_PROXIMITY_AUTH_REMOTE_DEVICE_LIFE_CYCLE_H_
#define CHROMEOS_COMPONENTS_PROXIMITY_AUTH_REMOTE_DEVICE_LIFE_CYCLE_H_

#include <ostream>

#include "base/macros.h"
#include "components/cryptauth/connection.h"
#include "components/cryptauth/remote_device.h"

namespace proximity_auth {

class Messenger;

// Controls the life cycle of connecting and authenticating to a remote device.
// After the life cycle is started, it can be in the following states:
//   FINDING_CONNECTION:
//       Continuiously attempts to create a connection to the remote device.
//       After connecting, transitions to the AUTHENTICATING state.
//   AUTHENTICATING:
//       Verifies that the connected device has the correct credentials. On
//       success, transitions to SECURE_CHANNEL_ESTABLISHED; otherwise,
//       transitions to AUTHENTICATION_FAILED.
//   SECURE_CHANNEL_ESTABLISHED:
//       Can send and receive messages securely from the remote device. Upon
//       disconnection, transitions to FINDING_CONNECTION.
//   AUTHENTICATION_FAILED:
//       Recovery state after authentication fails. After a brief wait,
//       transition to FINDING_CONNECTION.
// To stop the life cycle and clean up the connection, simply destroying this
// object.
class RemoteDeviceLifeCycle {
 public:
  // The possible states in the life cycle.
  enum class State {
    STOPPED,
    FINDING_CONNECTION,
    AUTHENTICATING,
    SECURE_CHANNEL_ESTABLISHED,
    AUTHENTICATION_FAILED,
  };

  // Interface for observing changes to the life cycle.
  class Observer {
   public:
    virtual ~Observer() {}

    // Called when the state in the life cycle changes.
    virtual void OnLifeCycleStateChanged(State old_state, State new_state) = 0;
  };

  virtual ~RemoteDeviceLifeCycle() {}

  // Starts the life cycle.
  virtual void Start() = 0;

  // Returns the RemoteDevice instance that this life cycle manages.
  virtual cryptauth::RemoteDevice GetRemoteDevice() const = 0;

  // Returns the current Connection, or null if the device is not yet connected.
  virtual cryptauth::Connection* GetConnection() const = 0;

  // Returns the current state of in the life cycle.
  virtual State GetState() const = 0;

  // Returns the client for sending and receiving messages. This function will
  // only return an instance if the state is SECURE_CHANNEL_ESTABLISHED;
  // otherwise, it will return nullptr.
  virtual Messenger* GetMessenger() = 0;

  // Adds an observer.
  virtual void AddObserver(Observer* observer) = 0;

  // Removes an observer.
  virtual void RemoveObserver(Observer* observer) = 0;
};

std::ostream& operator<<(std::ostream& stream,
                         const RemoteDeviceLifeCycle::State& state);

}  // namespace proximity_auth

#endif  // CHROMEOS_COMPONENTS_PROXIMITY_AUTH_REMOTE_DEVICE_LIFE_CYCLE_H_
