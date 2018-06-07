#include "inspector_io.h"

#include "inspector_socket_server.h"
#include "inspector/node_string.h"
#include "env-inl.h"
#include "debug_utils.h"
#include "node.h"
#include "node_crypto.h"
#include "node_mutex.h"
#include "v8-inspector.h"
#include "util.h"
#include "zlib.h"

#include <sstream>
#include <unicode/unistr.h>

#include <string.h>
#include <vector>


namespace node {
namespace inspector {
namespace {
using AsyncAndAgent = std::pair<uv_async_t, Agent*>;
using v8_inspector::StringBuffer;
using v8_inspector::StringView;

template <typename Transport>
using TransportAndIo = std::pair<Transport*, InspectorIo*>;

std::string ScriptPath(uv_loop_t* loop, const std::string& script_name) {
  std::string script_path;

  if (!script_name.empty()) {
    uv_fs_t req;
    req.ptr = nullptr;
    if (0 == uv_fs_realpath(loop, &req, script_name.c_str(), nullptr)) {
      CHECK_NOT_NULL(req.ptr);
      script_path = std::string(static_cast<char*>(req.ptr));
    }
    uv_fs_req_cleanup(&req);
  }

  return script_path;
}

// UUID RFC: https://www.ietf.org/rfc/rfc4122.txt
// Used ver 4 - with numbers
std::string GenerateID() {
  uint16_t buffer[8];
  CHECK(crypto::EntropySource(reinterpret_cast<unsigned char*>(buffer),
                              sizeof(buffer)));

  char uuid[256];
  snprintf(uuid, sizeof(uuid), "%04x%04x-%04x-%04x-%04x-%04x%04x%04x",
           buffer[0],  // time_low
           buffer[1],  // time_mid
           buffer[2],  // time_low
           (buffer[3] & 0x0fff) | 0x4000,  // time_hi_and_version
           (buffer[4] & 0x3fff) | 0x8000,  // clk_seq_hi clk_seq_low
           buffer[5],  // node
           buffer[6],
           buffer[7]);
  return uuid;
}

void HandleSyncCloseCb(uv_handle_t* handle) {
  *static_cast<bool*>(handle->data) = true;
}

void CloseAsyncAndLoop(uv_async_t* async) {
  bool is_closed = false;
  async->data = &is_closed;
  uv_close(reinterpret_cast<uv_handle_t*>(async), HandleSyncCloseCb);
  while (!is_closed)
    uv_run(async->loop, UV_RUN_ONCE);
  async->data = nullptr;
  CheckedUvLoopClose(async->loop);
}

// Delete main_thread_req_ on async handle close
void ReleasePairOnAsyncClose(uv_handle_t* async) {
  std::unique_ptr<AsyncAndAgent> pair(node::ContainerOf(&AsyncAndAgent::first,
      reinterpret_cast<uv_async_t*>(async)));
  // Unique_ptr goes out of scope here and pointer is deleted.
}

}  // namespace

std::unique_ptr<StringBuffer> Utf8ToStringView(const std::string& message) {
  icu::UnicodeString utf16 =
    icu::UnicodeString::fromUTF8(icu::StringPiece(message.data(),
      message.length()));
  StringView view(reinterpret_cast<const uint16_t*>(utf16.getBuffer()),
                  utf16.length());
  return StringBuffer::create(view);
}


class IoSessionDelegate : public InspectorSessionDelegate {
 public:
  explicit IoSessionDelegate(InspectorIo* io, int id) : io_(io), id_(id) { }
  void SendMessageToFrontend(const v8_inspector::StringView& message) override;
 private:
  InspectorIo* io_;
  int id_;
};

// Passed to InspectorSocketServer to handle WS inspector protocol events,
// mostly session start, message received, and session end.
class InspectorIoDelegate: public node::inspector::SocketServerDelegate {
 public:
  InspectorIoDelegate(InspectorIo* io, const std::string& target_id,
                      const std::string& script_path,
                      const std::string& script_name, bool wait);
  ~InspectorIoDelegate() {
    io_->ServerDone();
  }
  // Calls PostIncomingMessage() with appropriate InspectorAction:
  //   kStartSession
  void StartSession(int session_id, const std::string& target_id) override;
  //   kSendMessage
  void MessageReceived(int session_id, const std::string& message) override;
  //   kEndSession
  void EndSession(int session_id) override;

  std::vector<std::string> GetTargetIds() override;
  std::string GetTargetTitle(const std::string& id) override;
  std::string GetTargetUrl(const std::string& id) override;

  void AssignServer(InspectorSocketServer* server) override {
    server_ = server;
  }

 private:
  InspectorIo* io_;
  int session_id_;
  const std::string script_name_;
  const std::string script_path_;
  const std::string target_id_;
  bool waiting_;
  InspectorSocketServer* server_;
};

void InterruptCallback(v8::Isolate*, void* agent) {
  InspectorIo* io = static_cast<Agent*>(agent)->io();
  if (io != nullptr)
    io->DispatchMessages();
}

class DispatchMessagesTask : public v8::Task {
 public:
  explicit DispatchMessagesTask(Agent* agent) : agent_(agent) {}

  void Run() override {
    InspectorIo* io = agent_->io();
    if (io != nullptr)
      io->DispatchMessages();
  }

 private:
  Agent* agent_;
};

InspectorIo::InspectorIo(Environment* env, v8::Platform* platform,
                         const std::string& path, const DebugOptions& options,
                         bool wait_for_connect)
                         : options_(options), thread_(), state_(State::kNew),
                           parent_env_(env), thread_req_(), platform_(platform),
                           dispatching_messages_(false), script_name_(path),
                           wait_for_connect_(wait_for_connect), port_(-1),
                           id_(GenerateID()) {
  main_thread_req_ = new AsyncAndAgent({uv_async_t(), env->inspector_agent()});
  CHECK_EQ(0, uv_async_init(env->event_loop(), &main_thread_req_->first,
                            InspectorIo::MainThreadReqAsyncCb));
  uv_unref(reinterpret_cast<uv_handle_t*>(&main_thread_req_->first));
  CHECK_EQ(0, uv_sem_init(&thread_start_sem_, 0));
}

InspectorIo::~InspectorIo() {
  uv_sem_destroy(&thread_start_sem_);
  uv_close(reinterpret_cast<uv_handle_t*>(&main_thread_req_->first),
           ReleasePairOnAsyncClose);
}

bool InspectorIo::Start() {
  CHECK_EQ(state_, State::kNew);
  CHECK_EQ(uv_thread_create(&thread_, InspectorIo::ThreadMain, this), 0);
  uv_sem_wait(&thread_start_sem_);

  if (state_ == State::kError) {
    return false;
  }
  state_ = State::kAccepting;
  if (wait_for_connect_) {
    DispatchMessages();
  }
  return true;
}

void InspectorIo::Stop() {
  CHECK_IMPLIES(sessions_.empty(), state_ == State::kAccepting);
  Write(TransportAction::kKill, 0, StringView());
  int err = uv_thread_join(&thread_);
  CHECK_EQ(err, 0);
  state_ = State::kShutDown;
  DispatchMessages();
}

bool InspectorIo::IsStarted() {
  return platform_ != nullptr;
}

void InspectorIo::WaitForDisconnect() {
  if (state_ == State::kAccepting)
    state_ = State::kDone;
  if (!sessions_.empty()) {
    state_ = State::kShutDown;
    Write(TransportAction::kStop, 0, StringView());
    fprintf(stderr, "Waiting for the debugger to disconnect...\n");
    fflush(stderr);
  }
}

// static
void InspectorIo::ThreadMain(void* io) {
  static_cast<InspectorIo*>(io)->ThreadMain<InspectorSocketServer>();
}

// static
template <typename Transport>
void InspectorIo::IoThreadAsyncCb(uv_async_t* async) {
  TransportAndIo<Transport>* transport_and_io =
      static_cast<TransportAndIo<Transport>*>(async->data);
  if (transport_and_io == nullptr) {
    return;
  }
  Transport* transport = transport_and_io->first;
  InspectorIo* io = transport_and_io->second;
  MessageQueue<TransportAction> outgoing_message_queue;
  io->SwapBehindLock(&io->outgoing_message_queue_, &outgoing_message_queue);
  for (const auto& outgoing : outgoing_message_queue) {
    int session_id = std::get<1>(outgoing);
    switch (std::get<0>(outgoing)) {
    case TransportAction::kKill:
      transport->TerminateConnections();
      // Fallthrough
    case TransportAction::kStop:
      transport->Stop();
      break;
    case TransportAction::kSendMessage:
      transport->Send(session_id,
                      protocol::StringUtil::StringViewToUtf8(
                          std::get<2>(outgoing)->string()));
      break;
    case TransportAction::kAcceptSession:
      transport->AcceptSession(session_id);
      break;
    case TransportAction::kDeclineSession:
      transport->DeclineSession(session_id);
      break;
    }
  }
}

template <typename Transport>
void InspectorIo::ThreadMain() {
  uv_loop_t loop;
  loop.data = nullptr;
  int err = uv_loop_init(&loop);
  CHECK_EQ(err, 0);
  thread_req_.data = nullptr;
  err = uv_async_init(&loop, &thread_req_, IoThreadAsyncCb<Transport>);
  CHECK_EQ(err, 0);
  std::string script_path = ScriptPath(&loop, script_name_);
  auto delegate = std::unique_ptr<InspectorIoDelegate>(
      new InspectorIoDelegate(this, id_, script_path, script_name_,
                              wait_for_connect_));
  Transport server(std::move(delegate), &loop, options_.host_name(),
                   options_.port());
  TransportAndIo<Transport> queue_transport(&server, this);
  thread_req_.data = &queue_transport;
  if (!server.Start()) {
    state_ = State::kError;  // Safe, main thread is waiting on semaphore
    CloseAsyncAndLoop(&thread_req_);
    uv_sem_post(&thread_start_sem_);
    return;
  }
  port_ = server.Port();  // Safe, main thread is waiting on semaphore.
  if (!wait_for_connect_) {
    uv_sem_post(&thread_start_sem_);
  }
  uv_run(&loop, UV_RUN_DEFAULT);
  thread_req_.data = nullptr;
  CheckedUvLoopClose(&loop);
}

template <typename ActionType>
bool InspectorIo::AppendMessage(MessageQueue<ActionType>* queue,
                                ActionType action, int session_id,
                                std::unique_ptr<StringBuffer> buffer) {
  Mutex::ScopedLock scoped_lock(state_lock_);
  bool trigger_pumping = queue->empty();
  queue->push_back(std::make_tuple(action, session_id, std::move(buffer)));
  return trigger_pumping;
}

template <typename ActionType>
void InspectorIo::SwapBehindLock(MessageQueue<ActionType>* vector1,
                                 MessageQueue<ActionType>* vector2) {
  Mutex::ScopedLock scoped_lock(state_lock_);
  vector1->swap(*vector2);
}

void InspectorIo::PostIncomingMessage(InspectorAction action, int session_id,
                                      const std::string& message) {
  if (AppendMessage(&incoming_message_queue_, action, session_id,
                    Utf8ToStringView(message))) {
    Agent* agent = main_thread_req_->second;
    v8::Isolate* isolate = parent_env_->isolate();
    platform_->CallOnForegroundThread(isolate,
                                      new DispatchMessagesTask(agent));
    isolate->RequestInterrupt(InterruptCallback, agent);
    CHECK_EQ(0, uv_async_send(&main_thread_req_->first));
  }
  Mutex::ScopedLock scoped_lock(state_lock_);
  incoming_message_cond_.Broadcast(scoped_lock);
}

std::vector<std::string> InspectorIo::GetTargetIds() const {
  return { id_ };
}

TransportAction InspectorIo::Attach(int session_id) {
  Agent* agent = parent_env_->inspector_agent();
  fprintf(stderr, "Debugger attached.\n");
  sessions_[session_id] = agent->Connect(std::unique_ptr<IoSessionDelegate>(
      new IoSessionDelegate(this, session_id)));
  return TransportAction::kAcceptSession;
}

void InspectorIo::DispatchMessages() {
  if (dispatching_messages_)
    return;
  dispatching_messages_ = true;
  bool had_messages = false;
  do {
    if (dispatching_message_queue_.empty())
      SwapBehindLock(&incoming_message_queue_, &dispatching_message_queue_);
    had_messages = !dispatching_message_queue_.empty();
    while (!dispatching_message_queue_.empty()) {
      MessageQueue<InspectorAction>::value_type task;
      std::swap(dispatching_message_queue_.front(), task);
      dispatching_message_queue_.pop_front();
      int id = std::get<1>(task);
      StringView message = std::get<2>(task)->string();
      switch (std::get<0>(task)) {
      case InspectorAction::kStartSession:
        Write(Attach(id), id, StringView());
        break;
      case InspectorAction::kStartSessionUnconditionally:
        Attach(id);
        break;
      case InspectorAction::kEndSession:
        sessions_.erase(id);
        if (!sessions_.empty())
          continue;
        if (state_ == State::kShutDown) {
          state_ = State::kDone;
        } else {
          state_ = State::kAccepting;
        }
        break;
      case InspectorAction::kSendMessage:
        auto session = sessions_.find(id);
        if (session != sessions_.end() && session->second) {
          session->second->Dispatch(message);
        }
        break;
      }
    }
  } while (had_messages);
  dispatching_messages_ = false;
}

// static
void InspectorIo::MainThreadReqAsyncCb(uv_async_t* req) {
  AsyncAndAgent* pair = node::ContainerOf(&AsyncAndAgent::first, req);
  // Note that this may be called after io was closed or even after a new
  // one was created and ran.
  InspectorIo* io = pair->second->io();
  if (io != nullptr)
    io->DispatchMessages();
}

void InspectorIo::Write(TransportAction action, int session_id,
                        const StringView& inspector_message) {
  AppendMessage(&outgoing_message_queue_, action, session_id,
                StringBuffer::create(inspector_message));
  int err = uv_async_send(&thread_req_);
  CHECK_EQ(0, err);
}

bool InspectorIo::WaitForFrontendEvent() {
  // We allow DispatchMessages reentry as we enter the pause. This is important
  // to support debugging the code invoked by an inspector call, such
  // as Runtime.evaluate
  dispatching_messages_ = false;
  Mutex::ScopedLock scoped_lock(state_lock_);
  if (sessions_.empty())
    return false;
  if (dispatching_message_queue_.empty() && incoming_message_queue_.empty()) {
    incoming_message_cond_.Wait(scoped_lock);
  }
  return true;
}

InspectorIoDelegate::InspectorIoDelegate(InspectorIo* io,
                                         const std::string& target_id,
                                         const std::string& script_path,
                                         const std::string& script_name,
                                         bool wait)
                                         : io_(io),
                                           session_id_(0),
                                           script_name_(script_name),
                                           script_path_(script_path),
                                           target_id_(target_id),
                                           waiting_(wait),
                                           server_(nullptr) { }


void InspectorIoDelegate::StartSession(int session_id,
                                       const std::string& target_id) {
  session_id_ = session_id;
  InspectorAction action = InspectorAction::kStartSession;
  if (waiting_) {
    action = InspectorAction::kStartSessionUnconditionally;
    server_->AcceptSession(session_id);
  }
  io_->PostIncomingMessage(action, session_id, "");
}

void InspectorIoDelegate::MessageReceived(int session_id,
                                          const std::string& message) {
  // TODO(pfeldman): Instead of blocking execution while debugger
  // engages, node should wait for the run callback from the remote client
  // and initiate its startup. This is a change to node.cc that should be
  // upstreamed separately.
  if (waiting_) {
    if (message.find("\"Runtime.runIfWaitingForDebugger\"") !=
        std::string::npos) {
      waiting_ = false;
      io_->ResumeStartup();
    }
  }
  io_->PostIncomingMessage(InspectorAction::kSendMessage, session_id,
                           message);
}

void InspectorIoDelegate::EndSession(int session_id) {
  io_->PostIncomingMessage(InspectorAction::kEndSession, session_id, "");
}

std::vector<std::string> InspectorIoDelegate::GetTargetIds() {
  return { target_id_ };
}

std::string InspectorIoDelegate::GetTargetTitle(const std::string& id) {
  return script_name_.empty() ? GetHumanReadableProcessName() : script_name_;
}

std::string InspectorIoDelegate::GetTargetUrl(const std::string& id) {
  return "file://" + script_path_;
}

void IoSessionDelegate::SendMessageToFrontend(
    const v8_inspector::StringView& message) {
  io_->Write(TransportAction::kSendMessage, id_, message);
}

}  // namespace inspector
}  // namespace node
