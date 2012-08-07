/// \file procs.cpp
/// Contains generic functions for managing processes.

#include "procs.h"
#include <string.h>
#include <sys/types.h>
#include <signal.h>

#ifdef __FreeBSD__
#include <sys/wait.h>
#else
#include <wait.h>
#endif
#include <errno.h>
#include <iostream>
#include <sys/types.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdlib.h>
#include <stdio.h>

std::map<pid_t, std::string> Util::Procs::plist;
std::map<pid_t, Util::TerminationNotifier> Util::Procs::exitHandlers;
bool Util::Procs::handler_set = false;

/// Used internally to capture child signals and update plist.
void Util::Procs::childsig_handler(int signum){
  if (signum != SIGCHLD){return;}
  int status;
  pid_t ret = waitpid(-1, &status, WNOHANG);
  if (ret == 0){//ignore, would block otherwise
    return;
  }else if(ret < 0){
#if DEBUG >= 3
    std::cerr << "SIGCHLD received, but no child died";
#endif
    return;
  }
  int exitcode;
  if (WIFEXITED(status)){
    exitcode = WEXITSTATUS(status);
  }else if (WIFSIGNALED(status)){
    exitcode = -WTERMSIG(status);
  }else{/* not possible */return;}

  #if DEBUG >= 1
  std::string pname = plist[ret];
  #endif
  plist.erase(ret);
  #if DEBUG >= 1
  if (isActive(pname)){
    Stop(pname);
  }else{
    //can this ever happen?
    std::cerr << "Process " << pname << " fully terminated." << std::endl;
  }
  #endif

  TerminationNotifier tn = exitHandlers[ret];
  exitHandlers.erase(ret);
  if (tn){
#if DEBUG >= 2
    std::cerr << "Calling termination handler for " << pname << std::endl;
#endif
    tn(ret, exitcode);
  }
}

/// Attempts to run the command cmd.
/// Replaces the current process - use after forking first!
/// This function will never return - it will either run the given
/// command or kill itself with return code 42.
void Util::Procs::runCmd(std::string & cmd){
  //split cmd into arguments
  //supports a maximum of 20 arguments
  char * tmp = (char*)cmd.c_str();
  char * tmp2 = 0;
  char * args[21];
  int i = 0;
  tmp2 = strtok(tmp, " ");
  args[0] = tmp2;
  while (tmp2 != 0 && (i < 20)){
    tmp2 = strtok(0, " ");
    ++i;
    args[i] = tmp2;
  }
  if (i == 20){args[20] = 0;}
  //execute the command
  execvp(args[0], args);
  #if DEBUG >= 1
  std::cerr << "Error running \"" << cmd << "\": " << strerror(errno) << std::endl;
  #endif
  _exit(42);
}

/// Starts a new process if the name is not already active.
/// \return 0 if process was not started, process PID otherwise.
/// \arg name Name for this process - only used internally.
/// \arg cmd Commandline for this process.
pid_t Util::Procs::Start(std::string name, std::string cmd){
  if (isActive(name)){return getPid(name);}
  if (!handler_set){
    struct sigaction new_action;
    new_action.sa_handler = Util::Procs::childsig_handler;
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    sigaction(SIGCHLD, &new_action, NULL);
    handler_set = true;
  }
  pid_t ret = fork();
  if (ret == 0){
    runCmd(cmd);
  }else{
    if (ret > 0){
      #if DEBUG >= 1
      std::cerr << "Process " << name << " started, PID " << ret << ": " << cmd << std::endl;
      #endif
      plist.insert(std::pair<pid_t, std::string>(ret, name));
    }else{
      #if DEBUG >= 1
      std::cerr << "Process " << name << " could not be started. fork() failed." << std::endl;
      #endif
      return 0;
    }
  }
  return ret;
}

/// Starts two piped processes if the name is not already active.
/// \return 0 if process was not started, sub (sending) process PID otherwise.
/// \arg name Name for this process - only used internally.
/// \arg cmd Commandline for sub (sending) process.
/// \arg cmd2 Commandline for main (receiving) process.
pid_t Util::Procs::Start(std::string name, std::string cmd, std::string cmd2){
  if (isActive(name)){return getPid(name);}
  if (!handler_set){
    struct sigaction new_action;
    new_action.sa_handler = Util::Procs::childsig_handler;
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    sigaction(SIGCHLD, &new_action, NULL);
    handler_set = true;
  }
  int pfildes[2];
  if (pipe(pfildes) == -1){
    #if DEBUG >= 1
    std::cerr << "Process " << name << " could not be started. Pipe creation failed." << std::endl;
    #endif
    return 0;
  }

  int devnull = open("/dev/null", O_RDWR);
  pid_t ret = fork();
  if (ret == 0){
    close(pfildes[0]);
    dup2(pfildes[1],STDOUT_FILENO);
    close(pfildes[1]);
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDERR_FILENO);
    runCmd(cmd);
  }else{
    if (ret > 0){
      plist.insert(std::pair<pid_t, std::string>(ret, name));
    }else{
      #if DEBUG >= 1
      std::cerr << "Process " << name << " could not be started. fork() failed." << std::endl;
      #endif
      close(pfildes[1]);
      close(pfildes[0]);
      return 0;
    }
  }

  pid_t ret2 = fork();
  if (ret2 == 0){
    close(pfildes[1]);
    dup2(pfildes[0],STDIN_FILENO);
    close(pfildes[0]);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    runCmd(cmd2);
  }else{
    if (ret2 > 0){
      #if DEBUG >= 1
        std::cerr << "Process " << name << " started, PIDs (" << ret << ", " << ret2 << "): " << cmd << " | " << cmd2 << std::endl;
      #endif
      plist.insert(std::pair<pid_t, std::string>(ret2, name));
    }else{
      #if DEBUG >= 1
      std::cerr << "Process " << name << " could not be started. fork() failed." << std::endl;
      #endif
      Stop(name);
      close(pfildes[1]);
      close(pfildes[0]);
      return 0;
    }
  }
  close(pfildes[1]);
  close(pfildes[0]);
  return ret;
}

/// Starts three piped processes if the name is not already active.
/// \return 0 if process was not started, sub (sending) process PID otherwise.
/// \arg name Name for this process - only used internally.
/// \arg cmd Commandline for sub (sending) process.
/// \arg cmd2 Commandline for sub (middle) process.
/// \arg cmd3 Commandline for main (receiving) process.
pid_t Util::Procs::Start(std::string name, std::string cmd, std::string cmd2, std::string cmd3){
  if (isActive(name)){return getPid(name);}
  if (!handler_set){
    struct sigaction new_action;
    new_action.sa_handler = Util::Procs::childsig_handler;
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    sigaction(SIGCHLD, &new_action, NULL);
    handler_set = true;
  }

  int pfildes[2];
  int pfildes2[2];
  if (pipe(pfildes) == -1){
    #if DEBUG >= 1
    std::cerr << "Process " << name << " could not be started. Pipe creation failed." << std::endl;
    #endif
    return 0;
  }
  if (pipe(pfildes2) == -1){
    #if DEBUG >= 1
    std::cerr << "Process " << name << " could not be started. Pipe creation failed." << std::endl;
    #endif
    return 0;
  }
  
  int devnull = open("/dev/null", O_RDWR);
  pid_t ret = fork();
  if (ret == 0){
    close(pfildes[0]);
    dup2(pfildes[1],STDOUT_FILENO);
    close(pfildes[1]);
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDERR_FILENO);
    close(pfildes2[1]);
    close(pfildes2[0]);
    runCmd(cmd);
  }else{
    if (ret > 0){
      plist.insert(std::pair<pid_t, std::string>(ret, name));
    }else{
      #if DEBUG >= 1
      std::cerr << "Process " << name << " could not be started. fork() failed." << std::endl;
      #endif
      close(pfildes[1]);
      close(pfildes[0]);
      close(pfildes2[1]);
      close(pfildes2[0]);
      return 0;
    }
  }
  
  pid_t ret2 = fork();
  if (ret2 == 0){
    close(pfildes[1]);
    close(pfildes2[0]);
    dup2(pfildes[0],STDIN_FILENO);
    close(pfildes[0]);
    dup2(pfildes2[1],STDOUT_FILENO);
    close(pfildes2[1]);
    dup2(devnull, STDERR_FILENO);
    runCmd(cmd2);
  }else{
    if (ret2 > 0){
      #if DEBUG >= 1
      std::cerr << "Process " << name << " started, PIDs (" << ret << ", " << ret2 << "): " << cmd << " | " << cmd2 << std::endl;
      #endif
      plist.insert(std::pair<pid_t, std::string>(ret2, name));
    }else{
      #if DEBUG >= 1
      std::cerr << "Process " << name << " could not be started. fork() failed." << std::endl;
      #endif
      Stop(name);
      close(pfildes[1]);
      close(pfildes[0]);
      close(pfildes2[1]);
      close(pfildes2[0]);
      return 0;
    }
  }
  close(pfildes[1]);
  close(pfildes[0]);

  pid_t ret3 = fork();
  if (ret3 == 0){
    close(pfildes[1]);
    close(pfildes[0]);
    close(pfildes2[1]);
    dup2(pfildes2[0],STDIN_FILENO);
    close(pfildes2[0]);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    runCmd(cmd3);
  }else{
    if (ret3 > 0){
      #if DEBUG >= 1
      std::cerr << "Process " << name << " started, PIDs (" << ret << ", " << ret2 << ", " << ret3 << "): " << cmd << " | " << cmd2 << " | " << cmd3 << std::endl;
      #endif
      plist.insert(std::pair<pid_t, std::string>(ret3, name));
    }else{
      #if DEBUG >= 1
      std::cerr << "Process " << name << " could not be started. fork() failed." << std::endl;
      #endif
      Stop(name);
      close(pfildes[1]);
      close(pfildes[0]);
      close(pfildes2[1]);
      close(pfildes2[0]);
      return 0;
    }
  }
  
  return ret3;
}

/// Starts a new process with given fds if the name is not already active.
/// \return 0 if process was not started, process PID otherwise.
/// \arg name Name for this process - only used internally.
/// \arg argv Command for this process.
/// \arg fdin Standard input file descriptor. If -1, fd is automatically allocated and written.
/// \arg fdout Standard output file descriptor. If -1, fd is automatically allocated and written.
/// \arg fderr Standard error file descriptor. If -1, fd is automatically allocated and written.
pid_t Util::Procs::StartPiped(std::string name, char * argv[], int * fdin, int * fdout, int * fderr){
  if (isActive(name)){return getPid(name);}
  pid_t pid;
  int pipein[2], pipeout[2], pipeerr[2];
  if (!handler_set){
    struct sigaction new_action;
    new_action.sa_handler = Util::Procs::childsig_handler;
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    sigaction(SIGCHLD, &new_action, NULL);
    handler_set = true;
  }
  if (*fdin == -1 && pipe(pipein) < 0){
    #if DEBUG >= 1
    std::cerr << "Pipe (in) creation failed for " << name << std::endl;
    #endif
    return 0;
  }
  if (*fdout == -1 && pipe(pipeout) < 0) {
    #if DEBUG >= 1
    std::cerr << "Pipe (out) creation failed for " << name << std::endl;
    #endif
    if (*fdin == -1){close(pipein[0]);close(pipein[1]);}
    return 0;
  }
  if (*fderr == -1 && pipe(pipeerr) < 0) {
    #if DEBUG >= 1
    std::cerr << "Pipe (err) creation failed for " << name << std::endl;
    #endif
    if (*fdin  == -1){close(pipein [0]);close(pipein [1]);}
    if (*fdout == -1){close(pipeout[0]);close(pipeout[1]);}
    return 0;
  }
  pid = fork();
  if (pid == 0){//child
    if (*fdin == -1){
      close(pipein[1]);// close unused write end
      dup2(pipein[0], STDIN_FILENO);
    }else{
      dup2(*fdin, STDIN_FILENO);
    }
    if (*fdout == -1){
      close(pipeout[0]);// close unused read end
      dup2(pipeout[1], STDOUT_FILENO);
    }else{
      dup2(*fdout, STDOUT_FILENO);
    }
    if (*fderr == -1){
      close(pipeerr[0]);// close unused read end
      dup2(pipeerr[1], STDERR_FILENO);
    }else{
      dup2(*fderr, STDERR_FILENO);
    }
    execvp(argv[0], argv);
    #if DEBUG >= 1
    perror("execvp failed");
    #endif
    exit(42);
  } else if (pid == -1){
    #if DEBUG >= 1
    std::cerr << "Failed to fork for pipe: " << name << std::endl;
    #endif
    if (*fdin  == -1){close(pipein [0]);close(pipein [1]);}
    if (*fdout == -1){close(pipeout[0]);close(pipeout[1]);}
    if (*fderr == -1){close(pipeerr[0]);close(pipeerr[1]);}
    return 0;
  } else{//parent
    #if DEBUG >= 1
    std::cerr << "Piped process " << name << " started";
    std::cerr << " in="  << (*fdin  == -1 ? pipein [1] : *fdin );
    std::cerr << " out=" << (*fdout == -1 ? pipeout[0] : *fdout);
    std::cerr << " err=" << (*fderr == -1 ? pipeerr[0] : *fderr);
    std::cerr << ", PID " << pid << ": " << argv[0] << std::endl;
    #endif
    if (*fdin == -1){
      close(pipein[0]);// close unused end end
      *fdin = pipein[1];
    }
    if (*fdout == -1){
      close(pipeout[1]);// close unused write end
      *fdout = pipeout[0];
    }
    if (*fderr == -1){
      close(pipeerr[1]);// close unused write end
      *fderr = pipeerr[0];
    }
    plist.insert(std::pair<pid_t, std::string>(pid, name));
  }
  return pid;
}


/// Stops the named process, if running.
/// \arg name (Internal) name of process to stop
void Util::Procs::Stop(std::string name){
  int max = 5;
  while (isActive(name)){
    Stop(getPid(name));
    max--;
    if (max <= 0){return;}
  }
}

/// Stops the process with this pid, if running.
/// \arg name The PID of the process to stop.
void Util::Procs::Stop(pid_t name){
  if (isActive(name)){
    kill(name, SIGTERM);
  }
}

/// (Attempts to) stop all running child processes.
void Util::Procs::StopAll(){
  std::map<pid_t, std::string>::iterator it;
  for (it = plist.begin(); it != plist.end(); it++){
    Stop((*it).first);
  }
}

/// Returns the number of active child processes.
int Util::Procs::Count(){
   return plist.size();
}

/// Returns true if a process by this name is currently active.
bool Util::Procs::isActive(std::string name){
  std::map<pid_t, std::string>::iterator it;
  for (it = plist.begin(); it != plist.end(); it++){
    if ((*it).second == name){return true;}
  }
  return false;
}

/// Returns true if a process with this PID is currently active.
bool Util::Procs::isActive(pid_t name){
  return (plist.count(name) == 1);
}

/// Gets PID for this named process, if active.
/// \return NULL if not active, process PID otherwise.
pid_t Util::Procs::getPid(std::string name){
  std::map<pid_t, std::string>::iterator it;
  for (it = plist.begin(); it != plist.end(); it++){
    if ((*it).second == name){return (*it).first;}
  }
  return 0;
}

/// Gets name for this process PID, if active.
/// \return Empty string if not active, name otherwise.
std::string Util::Procs::getName(pid_t name){
  if (plist.count(name) == 1){
    return plist[name];
  }
  return "";
}

/// Registers one notifier function for when a process indentified by PID terminates.
/// \return true if the notifier could be registered, false otherwise.
bool Util::Procs::SetTerminationNotifier(pid_t pid, TerminationNotifier notifier){
  if (plist.find(pid) != plist.end()){
    exitHandlers[pid] = notifier;
    return true;
  }
  return false;
}
