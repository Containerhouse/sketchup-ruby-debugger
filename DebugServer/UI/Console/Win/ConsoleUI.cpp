// SketchUp Ruby API Debugger. Copyright 2013 Trimble Navigation Ltd.
// Authors:
// - Bugra Barin
//
#include "./ConsoleUI.h"
#include "./ConsoleInputBuffer.h"

#include <DebugServer/IDebugServer.h>
#include <Common/BreakPoint.h>
#include <Common/StackFrame.h>

#include <boost/lexical_cast.hpp>

#include <fcntl.h>
#include <io.h>
#include <iomanip>
#include <iostream>
#include <regex>
#include <stdio.h>

namespace {

using namespace SketchUp::RubyDebugger;

void RedirectStdStreamToConsole(DWORD stdHandle, FILE* stdFile,
                                bool openModeRead) {
  auto lStdHandle = reinterpret_cast<intptr_t>(GetStdHandle(stdHandle));
  int hConHandle = _open_osfhandle(lStdHandle, _O_TEXT);
  FILE* fp = _fdopen(hConHandle, (openModeRead ? "r" : "w"));
  *stdFile = *fp;
  setvbuf(stdFile, NULL, _IONBF, 0);
}

void PrintHelp() {
  std::cout << std::endl << "Debugger help\n"
  "Commands\n"
  "  b[reak] file:line          set breakpoint to some position\n"
  "  b[reak]                    list breakpoints\n"
  "  del[ete]                   delete a breakpoint\n"
  "  c[ont]                     run until program ends or hits a breakpoint\n"
  "  s[tep]                     step (into methods) one line\n"
  "  s[tep] o[ut]               step out of the current method\n"
  "  n[ext]                     go over one line, stepping over methods\n"
  "  w[here]                    display frames\n"
  "  f[rame]                    alias for where\n"
  "  l[ist]                     list program\n"
  "  up                         move to higher frame\n"
  "  down                       move to lower frame\n"
  "  v[ar] g[lobal]             show global variables\n"
  "  v[ar] l[ocal]              show local variables\n"
//   "  v[ar] i[nstance] <object>  show instance variables of object\n"
//   "  v[ar] c[onst] <object>     show constants of object\n"
//   "  m[ethod] i[nstance] <obj>  show methods of object\n"
//   "  m[ethod] <class|module>    show instance methods of class or module\n"
  "  p expression               evaluate expression and print its value\n"
  "  h[elp]                     print this help\n"
  "  <everything else>          evaluate\n";
}

void WriteText(const char* message) {
  std::cout << std::endl << message << std::endl;
}

void WriteBreakPoint(const BreakPoint& bp) {
  std::cout << "  " << bp.index << " " << bp.file << ":" << bp.line << "\n";
}

void WriteBreakPoints(const std::vector<BreakPoint>& bps) {
  if (bps.empty()) {
    WriteText("No breakpoints");
  } else {
    WriteText("Breakpoints:");
    for (size_t i = 0; i < bps.size(); ++i) {
      const auto& bp = bps[i];
      WriteBreakPoint(bp);
    }
  }
}

void WriteVariables(const IDebugServer::VariablesVector& var_vec) {
  std::cout << std::endl;
  for (auto it = var_vec.cbegin(), ite = var_vec.cend(); it != ite; ++it) {
    std::cout << "  " << it->name << " => " << it->value << std::endl;
  }
}

} // end anonymous namespace

namespace SketchUp {
namespace RubyDebugger {

ConsoleUI::ConsoleUI() :
  server_will_continue_(false),
  server_can_continue_(false),
  need_server_response_(false),
  need_what_from_server_(NEED_NOTHING)
{}

void ConsoleUI::Initialize(IDebugServer* server,
                           const std::string& /*str_debugger*/) {
  server_ = server;
  console_thread_ =
      std::thread(std::bind(&ConsoleUI::ConsoleThreadFunc, this));
}

void ConsoleUI::WritePrompt() {
  std::cout << std::endl << "sudb ("
            << (server_will_continue_ || !server_->IsStopped() ?
               "running" : "stopped") << "): ";
}

void ConsoleUI::ConsoleThreadFunc() {
  // Allocate a console for this app
  AllocConsole();

  SetConsoleTitle(L"SketchUp Ruby Debugger");

  // Redirect unbuffered STDOUT to the console
  RedirectStdStreamToConsole(STD_OUTPUT_HANDLE, stdout, false);

  // Redirect unbuffered STDIN to the console
  RedirectStdStreamToConsole(STD_INPUT_HANDLE, stdin, true);

  // Make cout, wcout, cin, wcin, wcerr, cerr, wclog and clog point to console
  // as well
  std::ios::sync_with_stdio();

  std::cout << "SketchUp Ruby API Command Line Debugger" << std::endl;

  // Command loop
  std::string line_str;
  ConsoleInputBuffer ci;
  HANDLE hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  WritePrompt();
  while (true) {
    if(::WaitForSingleObject(hStdInput, INFINITE) == WAIT_OBJECT_0) {
      if (ci.ReadLine(line_str)) {
        if (EvaluateCommand(line_str)) {
          ConsoleInputBuffer::RecordHistory(line_str);
        }
      }
    }
  }
}

bool ConsoleUI::EvaluateCommand(const std::string& str_command) {
  bool is_legal_command = false;
  bool signal_server_can_continue = false;
  bool write_prompt = true;
  need_server_response_ = false;
  need_what_from_server_ = NEED_NOTHING;
  expression_to_evaluate_.clear();

  static const std::regex reg_brk_list("^\\s*b(?:reak)?$");
  static const std::regex reg_brk("^\\s*b(?:reak)?\\s+(?:(.+):)?([^.:]+)$");
  static const std::regex reg_brk_del("^\\s*del(?:ete)?(?:\\s+(\\d+))?$");
  static const std::regex reg_cont("^\\s*c(?:ont)?$");
  static const std::regex reg_help("^\\s*h(?:elp)?$");
  static const std::regex reg_where("^\\s*w(?:here)?$");
  static const std::regex reg_frame("^\\s*f(?:rame)?$");
  static const std::regex reg_step("^\\s*s(?:tep)?\\s?");
  static const std::regex reg_next("^\\s*n(?:ext)?$");
  static const std::regex reg_list("^\\s*l(?:ist)?$");
  static const std::regex reg_up("^\\s*up?$");
  static const std::regex reg_down("^\\s*down?$");
  static const std::regex reg_eval("^\\s*p\\s+");
  static const std::regex reg_var("^\\s*v(ar)?\\s+");

  std::smatch what;
  if (regex_match(str_command, reg_brk_list)) {
    // List breakpoints
    std::vector<BreakPoint> bps = server_->GetBreakPoints();
    WriteBreakPoints(bps);
    is_legal_command = true;
  } else if (regex_match(str_command, what, reg_brk_del)) {
    // Delete breakpoint(s)
    try {
      size_t index = boost::lexical_cast<size_t>(what[1]);
      if (!server_->RemoveBreakPoint(index)) {
        WriteText("Cannot remove breakpoint");
      }
      is_legal_command = true;
    } catch(boost::bad_lexical_cast&) {}
  } else if (regex_match(str_command, what, reg_brk)) {
    // Add breakpoint
    if (what.size() == 3) {
      BreakPoint bp;
      bp.file = what[1];
      try {
        bp.line = boost::lexical_cast<size_t>(what[2]);
        bp.enabled = true;
        if (server_->AddBreakPoint(bp)) {
          WriteText("Added breakpoint:");
          WriteBreakPoint(bp);
        } else {
          WriteText("Cannot add breakpoint");
        }
        is_legal_command = true;
      } catch(boost::bad_lexical_cast&) {}
    }
  } else if (regex_match(str_command, reg_cont)) {
    signal_server_can_continue = true;
    is_legal_command = true;
  } else if (regex_search(str_command, what, reg_step)) {
    std::string suffix = what.suffix();
    static const std::regex reg_out("^o(ut)?$");
    if (regex_match(suffix, reg_out)) {
      server_->StepOut();
    } else {
      server_->Step();
    }
    signal_server_can_continue = true;
    is_legal_command = true;
  } else if (regex_match(str_command, reg_next)) {
    server_->StepOver();
    signal_server_can_continue = true;
    is_legal_command = true;
  } else if (regex_match(str_command, reg_help)) {
    PrintHelp();
    is_legal_command = true;
  } else if (regex_match(str_command, reg_up)) {
    server_->ShiftActiveFrame(true);
    WriteFrames();
    is_legal_command = true;
  } else if (regex_match(str_command, reg_down)) {
    server_->ShiftActiveFrame(false);
    WriteFrames();
    is_legal_command = true;
  } else if (regex_match(str_command, reg_where) ||
             regex_match(str_command, reg_frame)) {
    WriteFrames();
    is_legal_command = true;
  } else if (regex_match(str_command, reg_list)) {
    WriteCodeLines();
    is_legal_command = true;
  } else if (regex_search(str_command, what, reg_eval)) {
    expression_to_evaluate_ = what.suffix();
    need_what_from_server_ = NEED_EVAL;
    is_legal_command = true;
    write_prompt = false;
    need_server_response_ = true;
  } else if (regex_search(str_command, what, reg_var)) {
    std::string suffix = what.suffix();
    static const std::regex reg_global("^g(lobal)?$");
    static const std::regex reg_local("^l(ocal)?$");
    if (regex_match(suffix, reg_global)) {
      need_what_from_server_ = NEED_GLOBAL_VARS;
    } else if (regex_match(suffix, reg_local)) {
      need_what_from_server_ = NEED_LOCAL_VARS;
    }
    if (need_what_from_server_ != NEED_NOTHING) {
      is_legal_command = true;
      need_server_response_ = true;
      write_prompt = false;
    }
  } else {
    if (!str_command.empty()) {
      expression_to_evaluate_ = str_command;
      is_legal_command = true;
      write_prompt = false;
      need_server_response_ = true;
    }
  }

  if (!is_legal_command) {
    WriteText("Illegal command");
  }
  if (need_server_response_) {
    // Notify the server thread so it gets a chance to respond.
    server_wait_cv_.notify_one();
  }
  if (signal_server_can_continue) {
    {
      std::lock_guard<std::mutex> lock(server_wait_mutex_);
      server_can_continue_ = true;
    }
    server_wait_cv_.notify_one();
    server_will_continue_ = true;
  }
  if (write_prompt) {
    std::unique_lock<std::mutex> lock(console_output_mutex_);
    WritePrompt();
  }
  server_will_continue_ = false;
  return is_legal_command;
}

void ConsoleUI::WriteFrames() {
  std::vector<StackFrame> frames = server_->GetStackFrames();
  size_t active_frame_index = server_->GetActiveFrameIndex();
  std::cout << std::endl;
  for (size_t i = 0; i < frames.size(); ++i) {
    std::string prefix = (i == active_frame_index) ? "--> " : "    ";
    std::cout << prefix << "#" << (i + 1) << " " << frames[i].name << std::endl;
  }
}

void ConsoleUI::WaitForContinue() {
  std::unique_lock<std::mutex> lock(server_wait_mutex_);
  server_can_continue_ = false;
  while (!server_can_continue_) {
    server_wait_cv_.wait(lock);
    // Check if server response is needed
    if (need_server_response_) {
      if (need_what_from_server_ == NEED_EVAL) {
        Variable eval_res = server_->EvaluateExpression(expression_to_evaluate_);
        std::unique_lock<std::mutex> lock(console_output_mutex_);
        WriteText(eval_res.value.c_str());
      } else if (need_what_from_server_ == NEED_GLOBAL_VARS) {
        IDebugServer::VariablesVector var_vec = server_->GetGlobalVariables();
        WriteVariables(var_vec);
      } else if (need_what_from_server_ == NEED_LOCAL_VARS) {
        IDebugServer::VariablesVector var_vec = server_->GetLocalVariables();
        WriteVariables(var_vec);
      }
      WritePrompt();
    }
  }
}

void ConsoleUI::Break(BreakPoint bp) {
  {
    std::unique_lock<std::mutex> lock(console_output_mutex_);
    std::cout << std::endl << "BreakPoint " << bp.index << " at " << bp.file
              << ":" << bp.line;
    WriteCurrentLine();
    WritePrompt();
  }
  WaitForContinue();
}

void ConsoleUI::Break(const std::string& file, size_t line) {
  {
    std::unique_lock<std::mutex> lock(console_output_mutex_);
    std::cout << std::endl << "Stopped at " << file << ":" << line;
    WriteCurrentLine();
    WritePrompt();
  }
  WaitForContinue();
}

void ConsoleUI::WriteCodeLines()
{
  auto code_lines = server_->GetCodeLines(0, 0);
  size_t current_line = server_->GetBreakLineNumber();
  std::cout << std::endl;
  for (size_t i = 0; i < code_lines.size(); ++i) {
    const auto& line_pair = code_lines[i];
    std::string prefix = (line_pair.first == current_line) ? "=>" : "  ";
    std::cout << prefix << std::setw(4) << line_pair.first << "  "
              << line_pair.second;
  }
}

void ConsoleUI::WriteCurrentLine()
{
  size_t current_line = server_->GetBreakLineNumber();
  auto code_lines = server_->GetCodeLines(current_line, current_line);
  if (!code_lines.empty()) {
    const auto& line_pair = code_lines.front();
    std::cout << std::endl << "Line " << line_pair.first << ": "
              << line_pair.second;
  }
}

} // end namespace RubyDebugger
} // end namespace SketchUp
