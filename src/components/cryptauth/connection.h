// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_CRYPTAUTH_CONNECTION_H_
#define COMPONENTS_CRYPTAUTH_CONNECTION_H_

#include <memory>
#include <ostream>

#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/observer_list.h"
#include "components/cryptauth/remote_device.h"

namespace cryptauth {

class ConnectionObserver;
class WireMessage;

// Base class representing a connection with a remote device, which is a
// persistent bidirectional channel for sending and receiving wire messages.
class Connection {
 public:
  enum class Status {
    DISCONNECTED,
    IN_PROGRESS,
    CONNECTED,
  };

  // Constructs a connection to the given |remote_device|.
  explicit Connection(const RemoteDevice& remote_device);
  virtual ~Connection();

  // Returns true iff the connection's status is CONNECTED.
  bool IsConnected() const;

  // Returns true iff the connection is currently sending a message.
  bool is_sending_message() const { return is_sending_message_; }

  // Sends a message to the remote device.
  // |OnSendCompleted()| will be called for all observers upon completion with
  // either success or failure.
  void SendMessage(std::unique_ptr<WireMessage> message);

  virtual void AddObserver(ConnectionObserver* observer);
  virtual void RemoveObserver(ConnectionObserver* observer);

  const RemoteDevice& remote_device() const {
    return remote_device_;
  }

  // Abstract methods that subclasses should implement:

  // Attempts to connect to the remote device if not already connected.
  virtual void Connect() = 0;

  // Disconnects from the remote device.
  virtual void Disconnect() = 0;

  // The bluetooth address of the connected device.
  virtual std::string GetDeviceAddress() = 0;

  Status status() const { return status_; }

 protected:
  // Sets the connection's status to |status|. If this is different from the
  // previous status, notifies observers of the change in status.
  // Virtual for testing.
  virtual void SetStatus(Status status);

  // Called after attempting to send bytes over the connection, whether the
  // message was successfully sent or not.
  // Virtual for testing.
  virtual void OnDidSendMessage(const WireMessage& message, bool success);

  // Called when bytes are read from the connection. There should not be a send
  // in progress when this function is called.
  // Virtual for testing.
  virtual void OnBytesReceived(const std::string& bytes);

  // Sends bytes over the connection. The implementing class should call
  // OnDidSendMessage() once the send succeeds or fails. At most one send will
  // be
  // in progress.
  virtual void SendMessageImpl(std::unique_ptr<WireMessage> message) = 0;

  // Deserializes the |recieved_bytes_| and returns the resulting WireMessage,
  // or NULL if the message is malformed. Sets |is_incomplete_message| to true
  // if the |serialized_message| does not have enough data to parse the header,
  // or if the message length encoded in the message header exceeds the size of
  // the |serialized_message|. Exposed for testing.
  virtual std::unique_ptr<WireMessage> DeserializeWireMessage(
      bool* is_incomplete_message);

  void NotifyGattCharacteristicsNotAvailable();

  // Returns a string describing the associated device for logging purposes.
  std::string GetDeviceInfoLogString();

 private:
  // The remote device corresponding to this connection.
  const RemoteDevice remote_device_;

  // The current status of the connection.
  Status status_;

  // The registered observers of the connection.
  base::ObserverList<ConnectionObserver> observers_;

  // A temporary buffer storing bytes received before a received message can be
  // fully constructed.
  std::string received_bytes_;

  // Whether a message is currently in the process of being sent.
  bool is_sending_message_;

  DISALLOW_COPY_AND_ASSIGN(Connection);
};

std::ostream& operator<<(std::ostream& stream,
                         const Connection::Status& status);

}  // namespace cryptauth

#endif  // COMPONENTS_CRYPTAUTH_CONNECTION_H_
