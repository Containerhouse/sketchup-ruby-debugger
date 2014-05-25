// SketchUp Ruby API Debugger. Copyright 2014 Trimble Navigation Ltd.
// Authors:
// - Orhun Birsoy
// - Bugra Barin
//
#include "./RDIP.h"

#include <DebugServer/IDebugServer.h>
#include <DebugServer/Log.h>
#include <Common/BreakPoint.h>
#include <Common/StackFrame.h>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>

#include <cassert>
#include <memory>
#include <regex>

namespace SketchUp {
namespace RubyDebugger {

class RDIP::Connection : public std::enable_shared_from_this<RDIP::Connection> {
public:
  Connection(boost::asio::io_service& service, int port, IDebugServer* server,
             std::condition_variable& serverWaitCond,
             std::mutex& serverWaitMutex,
             bool& serverCanContinue,
             std::function<void(void)>& serverResponse,
             std::function<void(void)>& processServerResponse);

  void wait();
  void stopAtBreakpoint(BreakPoint bp);
  void suspendAt(const std::string& file, size_t line);

private:
  void start(const boost::system::error_code& err);
  void handleCommand(const boost::system::error_code& err);
  void evaluateCommand(const std::string& cmd);
  void getVariables(bool local);
  void getInstanceVariables(size_t object_id);
  void evalExpression();
  void sendVariables(std::string kind);

private:
  boost::asio::ip::tcp::socket socket_;
  boost::asio::ip::tcp::acceptor acceptor_;
  boost::asio::streambuf read_buffer_;
  boost::asio::streambuf write_buffer_;
  IDebugServer* server_;
  std::condition_variable &server_wait_cond_;
  std::mutex &server_wait_mutex_;
  bool &server_can_continue_;
  std::function<void(void)>& server_response_;
  std::function<void(void)>& process_server_response_;
  std::string expression_to_eval_;
  std::mutex variables_to_send_mutex_;
  IDebugServer::VariablesVector variables_to_send_;
};

RDIP::RDIP()
  : signal_set_(io_service_, SIGINT, SIGTERM, SIGSEGV)
  , server_can_continue_(false)
{}

RDIP::~RDIP() {
  io_service_.stop();
  service_thread_.join();
}

void RDIP::Initialize(IDebugServer* server, const std::string& str_debugger) {
  server_ = server;
  
  // Parse the port number if given.
  int port = 1234;
  const std::regex reg_port("port=(\\d+)");
  std::smatch match;
  if (regex_search(str_debugger, match, reg_port)) {
    port = boost::lexical_cast<int>(match[1]);
  }

  // Start the i/o service thread.
  service_thread_ = std::thread(std::bind(&RDIP::RunService, this, port));
}

void RDIP::WaitForContinue() {
  std::unique_lock<std::mutex> lock(server_wait_mutex_);
  server_can_continue_ = false;
  while(!server_can_continue_) {
    if (server_response_) {
      server_response_();
      if (process_server_response_)
        io_service_.post(process_server_response_);
      server_response_ = nullptr;
    }
    server_wait_cond_.wait(lock);
  }
  Log("Let SketchUp start\n");
}

void RDIP::Break(BreakPoint bp) {
  io_service_.post(std::bind(&RDIP::Connection::stopAtBreakpoint, connection_.get(), bp));
  WaitForContinue();
}

void RDIP::Break(const std::string& file, size_t line) {
  io_service_.post(std::bind(&RDIP::Connection::suspendAt, connection_.get(), file, line));
  WaitForContinue();
}

void RDIP::RunService(int port) {
  signal_set_.async_wait(std::bind(&RDIP::HandleFatalFailure, this, std::placeholders::_1, std::placeholders::_2));
  connection_ = std::make_shared<Connection>(io_service_, port, server_,
      server_wait_cond_, server_wait_mutex_, server_can_continue_, server_response_,
      process_server_response_);
  connection_->wait();
  io_service_.run();
}

void RDIP::HandleFatalFailure(const boost::system::error_code& err, int signal)
{}

using boost::asio::ip::tcp;
using boost::asio::ip::address;

RDIP::Connection::Connection(boost::asio::io_service& service, int port,
                             IDebugServer* server,
                             std::condition_variable& serverWaitCond,
                             std::mutex& serverWaitMutex,
                             bool& serverCanContinue,
                             std::function<void(void)>& serverResponse,
                             std::function<void(void)>& processServerResponse)
  : socket_(service)
  , acceptor_(service, tcp::endpoint(tcp::v4(), port))
  , server_(server)
  , server_wait_cond_(serverWaitCond)
  , server_wait_mutex_(serverWaitMutex)
  , server_can_continue_(serverCanContinue)
  , server_response_(serverResponse)
  , process_server_response_(processServerResponse)
{}

void RDIP::Connection::wait() {
  acceptor_.async_accept(socket_, std::bind(&Connection::start, this, std::placeholders::_1));
}

void RDIP::Connection::start(const boost::system::error_code& err) {
  async_read_until(socket_, read_buffer_, "\n", std::bind(&Connection::handleCommand, this, std::placeholders::_1));
}

void RDIP::Connection::handleCommand(const boost::system::error_code& err) {
  if(!err) {
    std::ostringstream out;
    out << &read_buffer_;
    std::string str = out.str();
    Log("\nCommand from IDE => ");
    Log(str.c_str());
    std::vector<std::string> commands;
    boost::split(commands, str, boost::is_any_of(";"));
    for(const auto& cmd : commands) {
      evaluateCommand(boost::trim_copy(cmd));
    }
    //assert(write(mSocket, boost::asio::buffer("<message>some text</message>\n")) > 0);
    async_read_until(socket_, read_buffer_, "\n", std::bind(&Connection::handleCommand, this, std::placeholders::_1));
  } else {
    std::ostringstream os;
    os << err;
    Log(os.str().c_str());
  }
}

static std::string encodeXml(const std::string& str) {
  std::string encoded = boost::replace_all_copy(str, "&", "&amp;");
  boost::replace_all(encoded, "\"", "&quot;");
  boost::replace_all(encoded, "<", "&lt;");
  boost::replace_all(encoded, ">", "&gt;");
  boost::replace_all(encoded, "'", "&apos;");
  return encoded;
}

void RDIP::Connection::evaluateCommand(const std::string& cmd) {
  static const std::regex reg_brk("^\\s*b(?:reak)?\\s+(?:(.+):)?([^.:]+)$");
  static const std::regex reg_brk_del("^\\s*del(?:ete)?(?:\\s+(\\d+))?$");
  static const std::regex reg_start("^\\s*start$");
  static const std::regex reg_exit("^\\s*exit?$");
  static const std::regex reg_cont("^\\s*c(?:ont)?$");
  static const std::regex reg_where("^\\s*w(?:here)?$");
  static const std::regex reg_frame("^\\s*f(?:rame)? ([0-9]+)$");
  static const std::regex reg_step("^\\s*s(?:tep)?\\s?");
  static const std::regex reg_next("^\\s*n(?:ext)?$");
  static const std::regex reg_finish("^\\s*finish?$");
  static const std::regex reg_var_inspect("v inspect\\s+");
  static const std::regex reg_thr_lst("^\\s*th(?:read)? l(?:ist)?$");
  static const std::regex reg_var_local("^\\s*v(?:ar)? l(?:ocal)?$");
  static const std::regex reg_var_global("^\\s*v(?:ar)? g(?:lobal)?$");
  static const std::regex reg_var_instance("^\\s*v(?:ar)? i(?:nstance)? (.+)$");

  std::smatch what;
  if(regex_match(cmd, what, reg_brk)) {
    if(what.size() == 3) {
      BreakPoint bp;
      bp.file = what[1];
      boost::replace_all(bp.file, "\\", "/");
      std::string s2 = what[2];
      bp.line = std::atoi(s2.c_str());
      bp.enabled = true;
      if(server_->AddBreakPoint(bp, true)) {
        std::ostringstream reply;
        reply << "<breakpointAdded no=\"" << bp.index << "\" location=\"" << bp.file << ":" << bp.line << "\"/>\n";
        write(socket_, boost::asio::buffer(reply.str()));
        Log(reply.str().c_str());
        Log("    => Breakpoint added\n");
      } else {
        Log("Adding breakpoint failed\n.");
      }
    }
  } else if (regex_match(cmd, what, reg_brk_del)) {
    if (what.size() == 2) {
      size_t bp_index = boost::lexical_cast<size_t>(what[1]);
      if (server_->RemoveBreakPoint(bp_index)) {
        std::ostringstream reply;
        reply << "<breakpointDeleted no=\"" << bp_index << "\" />\n";
        write(socket_, boost::asio::buffer(reply.str()));
        Log(reply.str().c_str());
        Log("    => Breakpoint deleted\n");
      } else {
        Log("Breakpoint could not be deleted\n");
      }
    }
  } else if(regex_match(cmd, what, reg_start) || 
            regex_match(cmd, what, reg_cont)) {
    std::lock_guard<std::mutex> lock(server_wait_mutex_);
    server_can_continue_ = true;
    server_wait_cond_.notify_all();
  } else if(regex_match(cmd, what, reg_exit)) {
    // Stop debugging. First let SU continue in case it's at a breakpoint.
    std::lock_guard<std::mutex> lock(server_wait_mutex_);
    server_can_continue_ = true;
    server_wait_cond_.notify_all();
    // Now call Stop. It's unclear if it is ok to do this from the RDIP thread
    // but it appears to work.
    server_->Stop();
  } else if(regex_match(cmd, what, reg_where)) {
    auto frames = server_->GetStackFrames();
    std::string str_send = "<frames>\n";

    size_t activeFrameIdx = server_->GetActiveFrameIndex();
    for(size_t i = 0; i < frames.size(); ++i) {
      const StackFrame& frame = frames[i];
      std::string file = encodeXml(frame.file);
      if(activeFrameIdx != i) {
        boost::format fmt("<frame no=\"%1%\" file=\"%2%\" line=\"%3%\"/>");
        fmt % i % file % frame.line;
        str_send += fmt.str();
      } else {
        boost::format fmt("<frame no=\"%1%\" file=\"%2%\" line=\"%3%\" current=\"yes\"/>");
        fmt % i % file % frame.line;
        str_send += fmt.str();
      }
    }
    str_send += "</frames>\n";
    Log(str_send.c_str());
    write(socket_, boost::asio::buffer(str_send));
  } else if(regex_match(cmd, what, reg_thr_lst)) {
    std::string str_send = "<threads>\n";
    std::ostringstream reply;
    reply << "<thread id=\"1\" status=\"run\"/>\n";
    reply << "</threads>\n";
    str_send += reply.str();
    write(socket_, boost::asio::buffer(str_send));
  } else if(regex_match(cmd, what, reg_frame)) {
    if(what.size() == 2) {
      size_t frameIndex = boost::lexical_cast<size_t>(what[1]);
      server_->SetActiveFrameIndex(frameIndex);
    }
  }
  else if(regex_match(cmd, what, reg_step)) {
    server_->Step();
    std::lock_guard<std::mutex> lock(server_wait_mutex_);
    server_can_continue_ = true;
    server_wait_cond_.notify_all();
  } else if(regex_match(cmd, what, reg_finish)) {
    server_->StepOut();
    std::lock_guard<std::mutex> lock(server_wait_mutex_);
    server_can_continue_ = true;
    server_wait_cond_.notify_all();
  } else if(regex_match(cmd, what, reg_next)) {
    server_->StepOver();
    std::lock_guard<std::mutex> lock(server_wait_mutex_);
    server_can_continue_ = true;
    server_wait_cond_.notify_all();
  } else if(regex_search(cmd, what, reg_var_inspect)) {
    expression_to_eval_ = what.suffix();
    server_response_ = std::bind(&RDIP::Connection::evalExpression, this);
    process_server_response_ = std::bind(&RDIP::Connection::sendVariables, this, "watch");
    server_wait_cond_.notify_all();
  } else if(regex_match(cmd, what, reg_var_local)) {
    // Local variables must be retrieved in the server thread. Wake it up
    // and have it call us.
    server_response_ = std::bind(&RDIP::Connection::getVariables, this, true);
    process_server_response_ = std::bind(&RDIP::Connection::sendVariables, this, "local");
    server_wait_cond_.notify_all();
  } else if(regex_match(cmd, what, reg_var_global)) {
    // Global variables must be retrieved in the server thread. Wake it up
    // and have it call us.
    server_response_ = std::bind(&RDIP::Connection::getVariables, this, false);
    process_server_response_ = std::bind(&RDIP::Connection::sendVariables, this, "global");
    server_wait_cond_.notify_all();
  } else if(regex_match(cmd, what, reg_var_instance)) {
    size_t objectID = 0;
    std::string str_what = what[1];
    sscanf(str_what.c_str(), "%x", &objectID);
    server_response_ = std::bind(&RDIP::Connection::getInstanceVariables, this, objectID);
    process_server_response_ = std::bind(&RDIP::Connection::sendVariables, this, "instance");
    server_wait_cond_.notify_all();
  } else {
    Log("Unknown command : ");
    Log(cmd.c_str());
    Log("\n");
  }
}

void RDIP::Connection::stopAtBreakpoint(BreakPoint bp) {
  std::ostringstream ss;
  ss << "<breakpoint file=\"" << bp.file << "\" line=\"" << bp.line << "\" threadId=\"1\"/>\n";
  Log("sending stopAtBreakpoint => ");

  auto str = ss.str();
  Log(str.c_str());
  write(socket_, boost::asio::buffer(str));
}

void RDIP::Connection::suspendAt(const std::string& file, size_t line) {
  std::ostringstream ss;
  ss << "<suspended file=\"" << encodeXml(file) << "\" line=\"" << line << "\" threadId=\"1\" frames=\"1\"/>\n";
  Log("sending suspendAt => ");

  auto str = ss.str();
  Log(str.c_str());
  write(socket_, boost::asio::buffer(str));
}

void RDIP::Connection::getVariables(bool local) {
  std::lock_guard<std::mutex> lock(variables_to_send_mutex_);
  variables_to_send_ = local ? server_->GetLocalVariables() :
                             server_->GetGlobalVariables();
}

void RDIP::Connection::getInstanceVariables(size_t object_id) {
  std::lock_guard<std::mutex> lock(variables_to_send_mutex_);
  variables_to_send_ = server_->GetInstanceVariables(object_id);
}

void RDIP::Connection::sendVariables(std::string kind) {
  std::lock_guard<std::mutex> lock(variables_to_send_mutex_);
  Log("sending variables\n");
  std::string send_str = "<variables>\n";
  for(const auto var : variables_to_send_) {
    boost::format fmt("<variable name=\"%s\" kind=\"%s\" value=\"%s\" type=\"%s\" hasChildren=\"%s\" objectId=\"%x\"/>\n");
    std::string value = encodeXml(var.value);
    std::string name = encodeXml(var.name);
    fmt % name % kind % value % var.type %
          (var.has_children ? "true" : "false") % var.object_id;
    send_str += fmt.str();
  }
  send_str += "</variables>\n";
  Log(send_str.c_str());
  write(socket_, boost::asio::buffer(send_str));
  variables_to_send_.clear();
}

void RDIP::Connection::evalExpression() {
  std::lock_guard<std::mutex> lock(variables_to_send_mutex_);
  variables_to_send_.clear();
  if (!expression_to_eval_.empty()) {
    Variable var = server_->EvaluateExpression(expression_to_eval_);
    variables_to_send_.push_back(var);
    expression_to_eval_.clear();
  }
}

} // end namespace RubyDebugger
} // end namespace SketchUp
