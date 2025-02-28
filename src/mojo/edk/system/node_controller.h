// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MOJO_EDK_SYSTEM_NODE_CONTROLLER_H_
#define MOJO_EDK_SYSTEM_NODE_CONTROLLER_H_

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/callback.h"
#include "base/containers/hash_tables.h"
#include "base/containers/queue.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/writable_shared_memory_region.h"
#include "base/task_runner.h"
#include "build/build_config.h"
#include "mojo/edk/embedder/scoped_platform_handle.h"
#include "mojo/edk/system/atomic_flag.h"
#include "mojo/edk/system/node_channel.h"
#include "mojo/edk/system/ports/event.h"
#include "mojo/edk/system/ports/name.h"
#include "mojo/edk/system/ports/node.h"
#include "mojo/edk/system/ports/node_delegate.h"
#include "mojo/edk/system/system_impl_export.h"

namespace base {
class PortProvider;
}

namespace mojo {
namespace edk {

class Broker;
class Core;
class MachPortRelay;

// The owner of ports::Node which facilitates core EDK implementation. All
// public interface methods are safe to call from any thread.
class MOJO_SYSTEM_IMPL_EXPORT NodeController : public ports::NodeDelegate,
                                               public NodeChannel::Delegate {
 public:
  class PortObserver : public ports::UserData {
   public:
    virtual void OnPortStatusChanged() = 0;

   protected:
    ~PortObserver() override {}
  };

  // |core| owns and out-lives us.
  explicit NodeController(Core* core);
  ~NodeController() override;

  const ports::NodeName& name() const { return name_; }
  Core* core() const { return core_; }
  ports::Node* node() const { return node_.get(); }
  scoped_refptr<base::TaskRunner> io_task_runner() const {
    return io_task_runner_;
  }

#if defined(OS_MACOSX) && !defined(OS_IOS)
  // Create the relay used to transfer mach ports between processes.
  void CreateMachPortRelay(base::PortProvider* port_provider);
#endif

  // Called exactly once, shortly after construction, and before any other
  // methods are called on this object.
  void SetIOTaskRunner(scoped_refptr<base::TaskRunner> io_task_runner);

  // Sends an invitation to a remote process (via |connection_params|) to join
  // this process's graph of connected processes as a broker client.
  void SendBrokerClientInvitation(
      base::ProcessHandle target_process,
      ConnectionParams connection_params,
      const std::vector<std::pair<std::string, ports::PortRef>>& attached_ports,
      const ProcessErrorCallback& process_error_callback);

  // Connects this node to the process which invited it to be a broker client.
  void AcceptBrokerClientInvitation(ConnectionParams connection_params);

  // Connects this node to a peer node. On success, |port| will be merged with
  // the corresponding port in the peer node. Returns an ID that can be used to
  // later close the connection with a call to ClosePeerConnection().
  uint64_t ConnectToPeer(ConnectionParams connection_params,
                         const ports::PortRef& port);

  // Close a connection to a peer associated with |peer_connection_id|.
  void ClosePeerConnection(uint64_t peer_connection_id);

  // Sets a port's observer. If |observer| is null the port's current observer
  // is removed.
  void SetPortObserver(const ports::PortRef& port,
                       scoped_refptr<PortObserver> observer);

  // Closes a port. Use this in lieu of calling Node::ClosePort() directly, as
  // it ensures the port's observer has also been removed.
  void ClosePort(const ports::PortRef& port);

  // Sends a message on a port to its peer.
  int SendUserMessage(const ports::PortRef& port_ref,
                      std::unique_ptr<ports::UserMessageEvent> message);

  // Merges a local port |port| into a port reserved by |name| in the node which
  // invited this node.
  void MergePortIntoInviter(const std::string& name,
                            const ports::PortRef& port);

  // Merges two local ports together.
  int MergeLocalPorts(const ports::PortRef& port0, const ports::PortRef& port1);

  // Creates a new shared buffer for use in the current process.
  base::WritableSharedMemoryRegion CreateSharedBuffer(size_t num_bytes);

  // Request that the Node be shut down cleanly. This may take an arbitrarily
  // long time to complete, at which point |callback| will be called.
  //
  // Note that while it is safe to continue using the NodeController's public
  // interface after requesting shutdown, you do so at your own risk and there
  // is NO guarantee that new messages will be sent or ports will complete
  // transfer.
  void RequestShutdown(const base::Closure& callback);

  // Notifies the NodeController that we received a bad message from the given
  // node.
  void NotifyBadMessageFrom(const ports::NodeName& source_node,
                            const std::string& error);

 private:
  friend Core;

  using NodeMap = std::unordered_map<ports::NodeName,
                                     scoped_refptr<NodeChannel>>;
  using OutgoingMessageQueue = base::queue<Channel::MessagePtr>;
  using PortMap = std::map<std::string, ports::PortRef>;

  struct PeerConnection {
    PeerConnection();
    PeerConnection(const PeerConnection& other);
    PeerConnection(PeerConnection&& other);
    PeerConnection(scoped_refptr<NodeChannel> channel,
                   const ports::PortRef& local_port,
                   uint64_t connection_id);
    ~PeerConnection();

    PeerConnection& operator=(const PeerConnection& other);
    PeerConnection& operator=(PeerConnection&& other);

    // NOTE: |channel| is null once the connection is fully established.
    scoped_refptr<NodeChannel> channel;
    ports::PortRef local_port;
    uint64_t connection_id;
  };

  void SendBrokerClientInvitationOnIOThread(
      base::ProcessHandle target_process,
      ConnectionParams connection_params,
      ports::NodeName token,
      const ProcessErrorCallback& process_error_callback);
  void AcceptBrokerClientInvitationOnIOThread(
      ConnectionParams connection_params);

  void ConnectToPeerOnIOThread(uint64_t peer_connection_id,
                               ConnectionParams connection_params,
                               ports::PortRef port);
  void ClosePeerConnectionOnIOThread(uint64_t peer_connection_id);

  scoped_refptr<NodeChannel> GetPeerChannel(const ports::NodeName& name);
  scoped_refptr<NodeChannel> GetInviterChannel();
  scoped_refptr<NodeChannel> GetBrokerChannel();

  void AddPeer(const ports::NodeName& name,
               scoped_refptr<NodeChannel> channel,
               bool start_channel);
  void DropPeer(const ports::NodeName& name, NodeChannel* channel);
  void SendPeerEvent(const ports::NodeName& name, ports::ScopedEvent event);
  void DropAllPeers();

  // ports::NodeDelegate:
  void ForwardEvent(const ports::NodeName& node,
                    ports::ScopedEvent event) override;
  void BroadcastEvent(ports::ScopedEvent event) override;
  void PortStatusChanged(const ports::PortRef& port) override;

  // NodeChannel::Delegate:
  void OnAcceptInvitee(const ports::NodeName& from_node,
                       const ports::NodeName& inviter_name,
                       const ports::NodeName& token) override;
  void OnAcceptInvitation(const ports::NodeName& from_node,
                          const ports::NodeName& token,
                          const ports::NodeName& invitee_name) override;
  void OnAddBrokerClient(const ports::NodeName& from_node,
                         const ports::NodeName& client_name,
                         base::ProcessHandle process_handle) override;
  void OnBrokerClientAdded(const ports::NodeName& from_node,
                           const ports::NodeName& client_name,
                           ScopedPlatformHandle broker_channel) override;
  void OnAcceptBrokerClient(const ports::NodeName& from_node,
                            const ports::NodeName& broker_name,
                            ScopedPlatformHandle broker_channel) override;
  void OnEventMessage(const ports::NodeName& from_node,
                      Channel::MessagePtr message) override;
  void OnRequestPortMerge(const ports::NodeName& from_node,
                          const ports::PortName& connector_port_name,
                          const std::string& token) override;
  void OnRequestIntroduction(const ports::NodeName& from_node,
                             const ports::NodeName& name) override;
  void OnIntroduce(const ports::NodeName& from_node,
                   const ports::NodeName& name,
                   ScopedPlatformHandle channel_handle) override;
  void OnBroadcast(const ports::NodeName& from_node,
                   Channel::MessagePtr message) override;
#if defined(OS_WIN) || (defined(OS_MACOSX) && !defined(OS_IOS))
  void OnRelayEventMessage(const ports::NodeName& from_node,
                           base::ProcessHandle from_process,
                           const ports::NodeName& destination,
                           Channel::MessagePtr message) override;
  void OnEventMessageFromRelay(const ports::NodeName& from_node,
                               const ports::NodeName& source_node,
                               Channel::MessagePtr message) override;
#endif
  void OnAcceptPeer(const ports::NodeName& from_node,
                    const ports::NodeName& token,
                    const ports::NodeName& peer_name,
                    const ports::PortName& port_name) override;
  void OnChannelError(const ports::NodeName& from_node,
                      NodeChannel* channel) override;
#if defined(OS_MACOSX) && !defined(OS_IOS)
  MachPortRelay* GetMachPortRelay() override;
#endif

  // Cancels all pending port merges. These are merges which are supposed to
  // be requested from the inviter ASAP, and they may be cancelled if the
  // connection to the inviter is broken or never established.
  void CancelPendingPortMerges();

  // Marks this NodeController for destruction when the IO thread shuts down.
  // This is used in case Core is torn down before the IO thread. Must only be
  // called on the IO thread.
  void DestroyOnIOThreadShutdown();

  // If there is a registered shutdown callback (meaning shutdown has been
  // requested, this checks the Node's status to see if clean shutdown is
  // possible. If so, shutdown is performed and the shutdown callback is run.
  void AttemptShutdownIfRequested();

  // These are safe to access from any thread as long as the Node is alive.
  Core* const core_;
  const ports::NodeName name_;
  const std::unique_ptr<ports::Node> node_;
  scoped_refptr<base::TaskRunner> io_task_runner_;

  // Guards |peers_|, |pending_peer_messages_|, and |next_peer_connection_id_|.
  base::Lock peers_lock_;

  // Channels to known peers, including inviter and invitees, if any.
  NodeMap peers_;

  // A unique ID generator for peer connections.
  uint64_t next_peer_connection_id_ = 1;

  // Outgoing message queues for peers we've heard of but can't yet talk to.
  std::unordered_map<ports::NodeName, OutgoingMessageQueue>
      pending_peer_messages_;

  // Guards |reserved_ports_|.
  base::Lock reserved_ports_lock_;

  // Ports reserved by name, per peer.
  std::map<ports::NodeName, PortMap> reserved_ports_;

  // Guards |pending_port_merges_| and |reject_pending_merges_|.
  base::Lock pending_port_merges_lock_;

  // A set of port merge requests awaiting inviter connection.
  std::vector<std::pair<std::string, ports::PortRef>> pending_port_merges_;

  // Indicates that new merge requests should be rejected because the inviter
  // has disconnected.
  bool reject_pending_merges_ = false;

  // Guards |inviter_name_| and |bootstrap_inviter_channel_|.
  base::Lock inviter_lock_;

  // The name of the node which invited us to join its network, if any.
  ports::NodeName inviter_name_;

  // A temporary reference to the inviter channel before we know their name.
  scoped_refptr<NodeChannel> bootstrap_inviter_channel_;

  // Guards |broker_name_|, |pending_broker_clients_|, and
  // |pending_relay_messages_|.
  base::Lock broker_lock_;

  // The name of our broker node, if any.
  ports::NodeName broker_name_;

  // A queue of remote broker clients waiting to be connected to the broker.
  base::queue<ports::NodeName> pending_broker_clients_;

  // Messages waiting to be relayed by the broker once it's known.
  std::unordered_map<ports::NodeName, OutgoingMessageQueue>
      pending_relay_messages_;

  // Guards |shutdown_callback_|.
  base::Lock shutdown_lock_;

  // Set by RequestShutdown(). If this is non-null, the controller will
  // begin polling the Node to see if clean shutdown is possible any time the
  // Node's state is modified by the controller.
  base::Closure shutdown_callback_;
  // Flag to fast-path checking |shutdown_callback_|.
  AtomicFlag shutdown_callback_flag_;

  // All other fields below must only be accessed on the I/O thread, i.e., the
  // thread on which core_->io_task_runner() runs tasks.

  // Channels to invitees during handshake.
  NodeMap pending_invitations_;

  std::map<ports::NodeName, PeerConnection> peer_connections_;

  // Maps from peer token to node name, pending or not.
  std::unordered_map<uint64_t, ports::NodeName> peer_connections_by_id_;

  // Indicates whether this object should delete itself on IO thread shutdown.
  // Must only be accessed from the IO thread.
  bool destroy_on_io_thread_shutdown_ = false;

#if !defined(OS_MACOSX) && !defined(OS_NACL_SFI) && !defined(OS_FUCHSIA)
  // Broker for sync shared buffer creation on behalf of broker clients.
  std::unique_ptr<Broker> broker_;
#endif

#if defined(OS_MACOSX) && !defined(OS_IOS)
  base::Lock mach_port_relay_lock_;
  // Relay for transferring mach ports to/from broker clients.
  std::unique_ptr<MachPortRelay> mach_port_relay_;
#endif

  DISALLOW_COPY_AND_ASSIGN(NodeController);
};

}  // namespace edk
}  // namespace mojo

#endif  // MOJO_EDK_SYSTEM_NODE_CONTROLLER_H_
