// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This file implements the Windows service controlling Me2Me host processes
// running within user sessions.

#include "remoting/host/win/wts_session_process_delegate.h"

#include <memory>
#include <utility>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/message_loop/message_loop_current.h"
#include "base/process/process_handle.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/win/scoped_handle.h"
#include "ipc/ipc_channel.h"
#include "ipc/ipc_channel_proxy.h"
#include "ipc/ipc_listener.h"
#include "ipc/ipc_message.h"
#include "mojo/edk/embedder/embedder.h"
#include "mojo/edk/embedder/named_platform_channel_pair.h"
#include "mojo/edk/embedder/outgoing_broker_client_invitation.h"
#include "mojo/edk/embedder/platform_channel_pair.h"
#include "mojo/edk/embedder/platform_handle_utils.h"
#include "mojo/edk/embedder/scoped_platform_handle.h"
#include "remoting/host/host_main.h"
#include "remoting/host/ipc_constants.h"
#include "remoting/host/switches.h"
#include "remoting/host/win/launch_process_with_token.h"
#include "remoting/host/win/security_descriptor.h"
#include "remoting/host/win/worker_process_launcher.h"
#include "remoting/host/win/wts_terminal_monitor.h"
#include "remoting/host/worker_process_ipc_delegate.h"

using base::win::ScopedHandle;

// Name of the default session desktop.
const char kDefaultDesktopName[] = "winsta0\\default";

namespace remoting {

// A private class actually implementing the functionality provided by
// |WtsSessionProcessDelegate|. This class is ref-counted and implements
// asynchronous fire-and-forget shutdown.
class WtsSessionProcessDelegate::Core
    : public base::RefCountedThreadSafe<Core>,
      public base::MessagePumpForIO::IOHandler,
      public IPC::Listener {
 public:
  Core(scoped_refptr<base::SingleThreadTaskRunner> io_task_runner,
       std::unique_ptr<base::CommandLine> target,
       bool launch_elevated,
       const std::string& channel_security);

  // Initializes the object returning true on success.
  bool Initialize(uint32_t session_id);

  // Stops the object asynchronously.
  void Stop();

  // Mirrors WorkerProcessLauncher::Delegate.
  void LaunchProcess(WorkerProcessLauncher* event_handler);
  void Send(IPC::Message* message);
  void CloseChannel();
  void KillProcess();

 private:
  friend class base::RefCountedThreadSafe<Core>;
  ~Core() override;

  // base::MessagePumpForIO::IOHandler implementation.
  void OnIOCompleted(base::MessagePumpForIO::IOContext* context,
                     DWORD bytes_transferred,
                     DWORD error) override;

  // IPC::Listener implementation.
  bool OnMessageReceived(const IPC::Message& message) override;
  void OnChannelConnected(int32_t peer_pid) override;
  void OnChannelError() override;

  // The actual implementation of LaunchProcess()
  void DoLaunchProcess();

  // Drains the completion port queue to make sure that all job object
  // notifications have been received.
  void DrainJobNotifications();

  // Notified that the completion port queue has been drained.
  void DrainJobNotificationsCompleted();

  // Creates and initializes the job object that will sandbox the launched child
  // processes.
  void InitializeJob(ScopedHandle job);

  // Notified that the job object initialization is complete.
  void InitializeJobCompleted(ScopedHandle job);

  // Called when the number of processes running in the job reaches zero.
  void OnActiveProcessZero();

  // Called when a process is launched in |job_|.
  void OnProcessLaunchDetected(base::ProcessId pid);

  void ReportFatalError();
  void ReportProcessLaunched(base::win::ScopedHandle worker_process,
                             mojo::edk::ScopedPlatformHandle server_handle);

  // The task runner all public methods of this class should be called on.
  scoped_refptr<base::SingleThreadTaskRunner> caller_task_runner_;

  // The task runner serving job object notifications.
  scoped_refptr<base::SingleThreadTaskRunner> io_task_runner_;

  // The server end of the IPC channel used to communicate to the worker
  // process.
  std::unique_ptr<IPC::ChannelProxy> channel_;

  // Security descriptor (as SDDL) to be applied to |channel_|.
  std::string channel_security_;

  WorkerProcessLauncher* event_handler_;

  // The job object used to control the lifetime of child processes.
  base::win::ScopedHandle job_;

  // True if the worker process should be launched elevated.
  bool launch_elevated_;

  // True if a laucnh attemp is pending.
  bool launch_pending_;

  // The token to be used to launch a process in a different session.
  base::win::ScopedHandle session_token_;

  // Command line of the launched process.
  std::unique_ptr<base::CommandLine> target_command_;

  // The handle of the worker process, if launched.
  base::win::ScopedHandle worker_process_;

  // If launching elevated, this holds the server handle after launch, until
  // the final process launches.
  mojo::edk::ScopedPlatformHandle elevated_server_handle_;

  // If launching elevated, this is the pid of the launcher process.
  base::ProcessId elevated_launcher_pid_ = base::kNullProcessId;

  // Tracks the id of the worker process.
  base::ProcessId worker_process_pid_ = base::kNullProcessId;

  // The pending process connection for the process being launched.
  std::unique_ptr<mojo::edk::OutgoingBrokerClientInvitation>
      broker_client_invitation_;

  DISALLOW_COPY_AND_ASSIGN(Core);
};

WtsSessionProcessDelegate::Core::Core(
    scoped_refptr<base::SingleThreadTaskRunner> io_task_runner,
    std::unique_ptr<base::CommandLine> target_command,
    bool launch_elevated,
    const std::string& channel_security)
    : caller_task_runner_(base::ThreadTaskRunnerHandle::Get()),
      io_task_runner_(io_task_runner),
      channel_security_(channel_security),
      event_handler_(nullptr),
      launch_elevated_(launch_elevated),
      launch_pending_(false),
      target_command_(std::move(target_command)) {}

bool WtsSessionProcessDelegate::Core::Initialize(uint32_t session_id) {
  DCHECK(caller_task_runner_->BelongsToCurrentThread());

  if (launch_elevated_) {
    // GetNamedPipeClientProcessId() is available starting from Vista.
    HMODULE kernel32 = ::GetModuleHandle(L"kernel32.dll");
    CHECK(kernel32 != nullptr);

    ScopedHandle job;
    job.Set(CreateJobObject(nullptr, nullptr));
    if (!job.IsValid()) {
      PLOG(ERROR) << "Failed to create a job object";
      return false;
    }

    // Limit the number of active processes in the job to two (the helper
    // process performing elevation and the worker process itself) and make sure
    // that all processes will be killed once the job object is destroyed.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
    memset(&info, 0, sizeof(info));
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    info.BasicLimitInformation.ActiveProcessLimit = 2;
    if (!SetInformationJobObject(job.Get(),
                                 JobObjectExtendedLimitInformation,
                                 &info,
                                 sizeof(info))) {
      PLOG(ERROR) << "Failed to set limits on the job object";
      return false;
    }

    // To receive job object notifications the job object is registered with
    // the completion port represented by |io_task_runner|. The registration has
    // to be done on the I/O thread because
    // MessageLoopForIO::RegisterJobObject() can only be called via
    // MessageLoopCurrentForIO::Get().
    io_task_runner_->PostTask(
        FROM_HERE, base::Bind(&Core::InitializeJob, this, base::Passed(&job)));
  }

  // Create a session token for the launched process.
  return CreateSessionToken(session_id, &session_token_);
}

void WtsSessionProcessDelegate::Core::Stop() {
  DCHECK(caller_task_runner_->BelongsToCurrentThread());

  KillProcess();

  // Drain the completion queue to make sure all job object notifications have
  // been received.
  DrainJobNotificationsCompleted();
}

void WtsSessionProcessDelegate::Core::LaunchProcess(
    WorkerProcessLauncher* event_handler) {
  DCHECK(caller_task_runner_->BelongsToCurrentThread());
  DCHECK(!event_handler_);

  event_handler_ = event_handler;
  DoLaunchProcess();
}

void WtsSessionProcessDelegate::Core::Send(IPC::Message* message) {
  DCHECK(caller_task_runner_->BelongsToCurrentThread());

  if (channel_) {
    channel_->Send(message);
  } else {
    delete message;
  }
}

void WtsSessionProcessDelegate::Core::CloseChannel() {
  DCHECK(caller_task_runner_->BelongsToCurrentThread());

  if (!channel_) {
    return;
  }

  channel_.reset();
  elevated_server_handle_.reset();
  elevated_launcher_pid_ = base::kNullProcessId;
  broker_client_invitation_.reset();
}

void WtsSessionProcessDelegate::Core::KillProcess() {
  DCHECK(caller_task_runner_->BelongsToCurrentThread());

  CloseChannel();

  event_handler_ = nullptr;
  launch_pending_ = false;

  if (launch_elevated_) {
    if (job_.IsValid()) {
      TerminateJobObject(job_.Get(), CONTROL_C_EXIT);
    }
  } else {
    if (worker_process_.IsValid()) {
      TerminateProcess(worker_process_.Get(), CONTROL_C_EXIT);
    }
  }

  worker_process_.Close();
}

WtsSessionProcessDelegate::Core::~Core() {
  DCHECK(!channel_);
  DCHECK(!event_handler_);
  DCHECK(!worker_process_.IsValid());
}

void WtsSessionProcessDelegate::Core::OnIOCompleted(
    base::MessagePumpForIO::IOContext* context,
    DWORD bytes_transferred,
    DWORD error) {
  DCHECK(io_task_runner_->BelongsToCurrentThread());

  // |bytes_transferred| is used in job object notifications to supply
  // the message ID; |context| carries process ID for the events we listen for.
  base::ProcessId process_id =
      static_cast<base::ProcessId>(reinterpret_cast<uintptr_t>(context));
  switch (bytes_transferred) {
    case JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO: {
      caller_task_runner_->PostTask(
          FROM_HERE, base::Bind(&Core::OnActiveProcessZero, this));
      break;
    }
    case JOB_OBJECT_MSG_NEW_PROCESS: {
      if (elevated_launcher_pid_ == base::kNullProcessId) {
        // Ignore process launch events when we don't have a valid launcher pid.
        return;
      }

      if (process_id != elevated_launcher_pid_) {
        DCHECK_EQ(worker_process_pid_, base::kNullProcessId);
        worker_process_pid_ = process_id;
      }
      break;
    }
    case JOB_OBJECT_MSG_EXIT_PROCESS: {
      if (process_id == worker_process_pid_) {
        // In official builds the first launch of a UiAccess enabled binary
        // will fail due to 'STATUS_ELEVATION_REQUIRED'.  This is an artifact of
        // using ShellExecuteEx() to launch the process.  In this scenario, we
        // will clear out the previously stored value for |worker_process_pid_|
        // and retry after the subsequent relaunch of the worker process.
        worker_process_pid_ = base::kNullProcessId;
      } else if (process_id == elevated_launcher_pid_) {
        if (worker_process_pid_ == base::kNullProcessId) {
          // The elevated launcher process can fail to launch without attemping
          // to launch the worker.  In this scenario, the failure will be
          // detected outside this method and the elevated launcher will be
          // launched again.
          return;
        }

        caller_task_runner_->PostTask(
            FROM_HERE, base::Bind(&Core::OnProcessLaunchDetected, this,
                                  worker_process_pid_));
      }
      break;
    }
  }
}

bool WtsSessionProcessDelegate::Core::OnMessageReceived(
    const IPC::Message& message) {
  DCHECK(caller_task_runner_->BelongsToCurrentThread());

  return event_handler_->OnMessageReceived(message);
}

void WtsSessionProcessDelegate::Core::OnChannelConnected(int32_t peer_pid) {
  DCHECK(caller_task_runner_->BelongsToCurrentThread());

  if (event_handler_)
    event_handler_->OnChannelConnected(peer_pid);
}

void WtsSessionProcessDelegate::Core::OnChannelError() {
  DCHECK(caller_task_runner_->BelongsToCurrentThread());

  event_handler_->OnChannelError();
}

void WtsSessionProcessDelegate::Core::DoLaunchProcess() {
  DCHECK(caller_task_runner_->BelongsToCurrentThread());
  DCHECK(!channel_);
  DCHECK(!worker_process_.IsValid());

  base::CommandLine command_line(target_command_->argv());
  if (launch_elevated_) {
    // The job object is not ready. Retry starting the host process later.
    if (!job_.IsValid()) {
      launch_pending_ = true;
      return;
    }

    // Construct the helper binary name.
    base::FilePath helper_binary;
    if (!GetInstalledBinaryPath(kHostBinaryName, &helper_binary)) {
      ReportFatalError();
      return;
    }

    // Create the command line passing the name of the IPC channel to use and
    // copying known switches from the caller's command line.
    command_line.SetProgram(helper_binary);
    command_line.AppendSwitchPath(kElevateSwitchName,
                                  target_command_->GetProgram());
  }

  std::string mojo_pipe_token = mojo::edk::GenerateRandomToken();
  broker_client_invitation_ =
      std::make_unique<mojo::edk::OutgoingBrokerClientInvitation>();
  std::unique_ptr<IPC::ChannelProxy> channel = IPC::ChannelProxy::Create(
      broker_client_invitation_->AttachMessagePipe(mojo_pipe_token).release(),
      IPC::Channel::MODE_SERVER, this, io_task_runner_,
      base::ThreadTaskRunnerHandle::Get());
  command_line.AppendSwitchASCII(kMojoPipeToken, mojo_pipe_token);

  std::unique_ptr<mojo::edk::PlatformChannelPair> normal_mojo_channel;
  std::unique_ptr<mojo::edk::NamedPlatformChannelPair> elevated_mojo_channel;
  base::HandlesToInheritVector handles_to_inherit;
  if (launch_elevated_) {
    // Pass the name of the IPC channel to use.
    mojo::edk::NamedPlatformChannelPair::Options options;
    options.security_descriptor = base::UTF8ToUTF16(channel_security_);
    elevated_mojo_channel =
        std::make_unique<mojo::edk::NamedPlatformChannelPair>(options);
    elevated_mojo_channel->PrepareToPassClientHandleToChildProcess(
        &command_line);
  } else {
    normal_mojo_channel = std::make_unique<mojo::edk::PlatformChannelPair>();
    normal_mojo_channel->PrepareToPassClientHandleToChildProcess(
        &command_line, &handles_to_inherit);
  }

  // Try to launch the process.
  ScopedHandle worker_process;
  ScopedHandle worker_thread;
  if (!LaunchProcessWithToken(
          command_line.GetProgram(), command_line.GetCommandLineString(),
          session_token_.Get(), /*security_attributes=*/nullptr,
          /* thread_attributes= */ nullptr, handles_to_inherit,
          /* creation_flags= */ CREATE_SUSPENDED | CREATE_BREAKAWAY_FROM_JOB,
          base::UTF8ToUTF16(kDefaultDesktopName).c_str(), &worker_process,
          &worker_thread)) {
    ReportFatalError();
    return;
  }

  if (launch_elevated_) {
    if (!AssignProcessToJobObject(job_.Get(), worker_process.Get())) {
      PLOG(ERROR) << "Failed to assign the worker to the job object";
      ReportFatalError();
      return;
    }
  }

  if (!ResumeThread(worker_thread.Get())) {
    PLOG(ERROR) << "Failed to resume the worker thread";
    ReportFatalError();
    return;
  }

  channel_ = std::move(channel);

  if (launch_elevated_) {
    // When launching an elevated worker process, an intermediate launcher
    // process launches the worker process. Reporting the launch waits until the
    // worker process launch is detected. Until then, store the values needed in
    // fields. See OnProcessLaunchDetected for their use.
    elevated_server_handle_ = elevated_mojo_channel->PassServerHandle();
    elevated_launcher_pid_ = GetProcessId(worker_process.Get());
    DCHECK(elevated_server_handle_.is_valid());
  } else {
    ReportProcessLaunched(std::move(worker_process),
                          normal_mojo_channel->PassServerHandle());
  }
}

void WtsSessionProcessDelegate::Core::DrainJobNotifications() {
  DCHECK(io_task_runner_->BelongsToCurrentThread());

  // DrainJobNotifications() is posted after the job object is destroyed, so
  // by this time all notifications from the job object have been processed
  // already. Let the main thread know that the queue has been drained.
  caller_task_runner_->PostTask(FROM_HERE, base::Bind(
      &Core::DrainJobNotificationsCompleted, this));
}

void WtsSessionProcessDelegate::Core::DrainJobNotificationsCompleted() {
  DCHECK(caller_task_runner_->BelongsToCurrentThread());

  if (job_.IsValid()) {
    job_.Close();

    // Drain the completion queue to make sure all job object notification have
    // been received.
    io_task_runner_->PostTask(FROM_HERE, base::Bind(
        &Core::DrainJobNotifications, this));
  }
}

void WtsSessionProcessDelegate::Core::InitializeJob(ScopedHandle job) {
  DCHECK(io_task_runner_->BelongsToCurrentThread());

  // Register to receive job notifications via the I/O thread's completion port.
  if (!base::MessageLoopCurrentForIO::Get()->RegisterJobObject(job.Get(),
                                                               this)) {
    PLOG(ERROR) << "Failed to associate the job object with a completion port";
    return;
  }

  // Let the main thread know that initialization is complete.
  caller_task_runner_->PostTask(FROM_HERE, base::Bind(
      &Core::InitializeJobCompleted, this, base::Passed(&job)));
}

void WtsSessionProcessDelegate::Core::InitializeJobCompleted(ScopedHandle job) {
  DCHECK(caller_task_runner_->BelongsToCurrentThread());
  DCHECK(!job_.IsValid());

  job_ = std::move(job);

  if (launch_pending_) {
    DoLaunchProcess();
  }
}

void WtsSessionProcessDelegate::Core::OnActiveProcessZero() {
  DCHECK(caller_task_runner_->BelongsToCurrentThread());

  if (launch_pending_) {
    LOG(ERROR) << "The worker process exited before connecting via IPC.";
    launch_pending_ = false;
    ReportFatalError();
  }
}

void WtsSessionProcessDelegate::Core::OnProcessLaunchDetected(
    base::ProcessId pid) {
  DCHECK(caller_task_runner_->BelongsToCurrentThread());
  DCHECK_NE(pid, elevated_launcher_pid_);

  if (!elevated_server_handle_.is_valid()) {
    return;
  }

  DWORD desired_access =
      SYNCHRONIZE | PROCESS_DUP_HANDLE | PROCESS_QUERY_INFORMATION;
  base::win::ScopedHandle worker_process(
      OpenProcess(desired_access, false, pid));
  if (!worker_process.IsValid()) {
    PLOG(ERROR) << "Failed to open process " << pid;
    ReportFatalError();
    return;
  }
  elevated_launcher_pid_ = base::kNullProcessId;
  ReportProcessLaunched(std::move(worker_process),
                        std::move(elevated_server_handle_));
}

void WtsSessionProcessDelegate::Core::ReportFatalError() {
  DCHECK(caller_task_runner_->BelongsToCurrentThread());

  CloseChannel();

  WorkerProcessLauncher* event_handler = event_handler_;
  event_handler_ = nullptr;
  event_handler->OnFatalError();
}

void WtsSessionProcessDelegate::Core::ReportProcessLaunched(
    base::win::ScopedHandle worker_process,
    mojo::edk::ScopedPlatformHandle server_handle) {
  DCHECK(caller_task_runner_->BelongsToCurrentThread());
  DCHECK(!worker_process_.IsValid());

  broker_client_invitation_->Send(
      worker_process.Get(),
      mojo::edk::ConnectionParams(mojo::edk::TransportProtocol::kLegacy,
                                  std::move(server_handle)));
  broker_client_invitation_.reset();
  worker_process_ = std::move(worker_process);

  // Report a handle that can be used to wait for the worker process completion,
  // query information about the process and duplicate handles.
  DWORD desired_access =
      SYNCHRONIZE | PROCESS_DUP_HANDLE | PROCESS_QUERY_INFORMATION;
  HANDLE temp_handle;
  if (!DuplicateHandle(GetCurrentProcess(), worker_process_.Get(),
                       GetCurrentProcess(), &temp_handle, desired_access, FALSE,
                       0)) {
    PLOG(ERROR) << "Failed to duplicate a handle";
    ReportFatalError();
    return;
  }
  ScopedHandle limited_handle(temp_handle);

  event_handler_->OnProcessLaunched(std::move(limited_handle));
}

WtsSessionProcessDelegate::WtsSessionProcessDelegate(
    scoped_refptr<base::SingleThreadTaskRunner> io_task_runner,
    std::unique_ptr<base::CommandLine> target_command,
    bool launch_elevated,
    const std::string& channel_security) {
  core_ = new Core(io_task_runner, std::move(target_command), launch_elevated,
                   channel_security);
}

WtsSessionProcessDelegate::~WtsSessionProcessDelegate() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  core_->Stop();
}

bool WtsSessionProcessDelegate::Initialize(uint32_t session_id) {
  return core_->Initialize(session_id);
}

void WtsSessionProcessDelegate::LaunchProcess(
    WorkerProcessLauncher* event_handler) {
  core_->LaunchProcess(event_handler);
}

void WtsSessionProcessDelegate::Send(IPC::Message* message) {
  core_->Send(message);
}

void WtsSessionProcessDelegate::CloseChannel() {
  core_->CloseChannel();
}

void WtsSessionProcessDelegate::KillProcess() {
  core_->KillProcess();
}

}  // namespace remoting
