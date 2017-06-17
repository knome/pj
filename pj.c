
// 
// pj, a very simple process jail
//

// ----------------------------------------------------
//               Modified BSD License
// ----------------------------------------------------
// 
// Copyright 2016 Michael Speer <knomenet@gmail.com>
// 
// Redistribution and use in source and binary forms,
// with or without modification, are permitted provided
// that the following conditions are met:
// 
// 1. Redistributions of source code must retain the
//    above copyright notice, this list of conditions
//    and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce
//    the above copyright notice, this list of
//    conditions and the following disclaimer in the
//    documentation and/or other materials provided
//    with the distribution.
// 
// 3. Neither the name of the copyright holder nor
//    the names of its contributors may be used to
//    endorse or promote products derived from this
//    software without specific prior written
//    permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS
// AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
// SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
// NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
// TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// ----------------------------------------------------

///// ///// /////

#define _POSIX_SOURCE
#define _BSD_SOURCE

#include <sys/prctl.h>
#include <stdio.h>
#include <signal.h>
#include <sys/wait.h>
#include <linux/wait.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <getopt.h>

///// ///// /////

#define PJ_PER_BATCH 256

///// ///// /////

struct Child ;

static int  setup_options          ( int, char **                    );
static void setup_signal_masks     ( void                            );
static void set_ourself_as_reaper  ( void                            );
static void setup_signal_handlers  ( void                            );
static void setup_handler          ( int, void(*)(int)               );
static void note_signal            ( int                             );
static void wait_on_signal         ( void                            );
static void pass_on_signal         ( int, int                        );
static int  find_children          ( void                            );
static int  stat_to_child          ( struct dirent *, struct Child * );
static int  isnum                  ( char *                          );
static int  kill_and_reap_children ( int                             );
static void reap_children          ( void                            );
static void kill_child             ( struct Child *, char *          );
static int  start_main_child       ( int, char **                    );

///// ///// /////

static int g_ourpid = 0 ;

static struct {
  int verbose         ;
  int showstats       ;
  int killOnSignal    ;
  int waitForChildren ;
} g_options = {
  .verbose         = 0,
  .showstats       = 0,
  .killOnSignal    = 0,
  .waitForChildren = 0,
};

static struct {
  volatile sig_atomic_t signalPending ;
  volatile sig_atomic_t  unknown      ;
  
  volatile sig_atomic_t sigChld ;
  volatile sig_atomic_t sigHup  ;
  volatile sig_atomic_t sigInt  ;
  volatile sig_atomic_t sigQuit ;
  volatile sig_atomic_t sigTerm ;
  volatile sig_atomic_t sigUsr1 ;
  volatile sig_atomic_t sigUsr2 ;
} g_received = {
.sigHup  = 0,
  .sigInt  = 0,
  .sigQuit = 0,
  .sigTerm = 0,
  .sigUsr1 = 0,
  .sigUsr2 = 0,
  .unknown = 0,
};

static sigset_t g_mask, g_oldmask ;

// this prevents the very minor chance you're using waitForChildren to allow daemons
// the original process tossed off to linger around, and one of them happens to spawn
// a process with the same pid as the original main child process causing the exit code
// to be overridden when it is reaped. this ensures that we only record the exit code
//  the first time
// 
static int g_mainChildPid = 0 ;
static int g_mainExit     = 0 ;
static int g_mainIsDone   = 0 ;

static struct Child {
  int pid    ;
  int ppid   ;
  char state ;
} g_children [ PJ_PER_BATCH ];

static struct {
  uint64_t reaped ;
  uint64_t killed ;
} g_stats = {
  .reaped = 0,
  .killed = 0,
};

///// ///// /////

int main( int argc, char ** argv ){
  
  int lastOptionIndex = setup_options( argc, argv );
  g_ourpid            = getpid();
  
  setup_signal_masks();
  set_ourself_as_reaper();
  setup_signal_handlers();
  
  g_mainChildPid = start_main_child( argc, &argv[ lastOptionIndex ] );
  
  while(1){
    wait_on_signal();
    
    if( g_received.unknown ){
      fprintf( stderr, "received unknown signal in callback\n" );
      fflush( stderr );
    }
    
    // if any non-sigchld signals are pending, handle them
    // 
    if( g_received.sigHup  || g_received.sigInt  || g_received.sigQuit
        ||
        g_received.sigTerm || g_received.sigUsr1 || g_received.sigUsr2
    ){
      if( g_options.killOnSignal ){
        goto kill_and_reap_children ;
      } else {
        // !killOnSignal, merely pass through signals instead      
        if( g_received.sigHup  ){ g_received.sigHup  = 0 ; pass_on_signal( g_mainChildPid, SIGHUP  ); }
        if( g_received.sigInt  ){ g_received.sigInt  = 0 ; pass_on_signal( g_mainChildPid, SIGINT  ); }
        if( g_received.sigQuit ){ g_received.sigQuit = 0 ; pass_on_signal( g_mainChildPid, SIGQUIT ); }
        if( g_received.sigTerm ){ g_received.sigTerm = 0 ; pass_on_signal( g_mainChildPid, SIGTERM ); }
        if( g_received.sigUsr1 ){ g_received.sigUsr1 = 0 ; pass_on_signal( g_mainChildPid, SIGUSR1 ); }
        if( g_received.sigUsr2 ){ g_received.sigUsr2 = 0 ; pass_on_signal( g_mainChildPid, SIGUSR2 ); }
      }
    }
    
    // if any sigchld signals are pending, wait on children until none remain
    if( g_received.sigChld ){
      g_received.sigChld = 0;
      
      reap_children();
    }
    
    // if the mainChild is done and we're not waiting for a signal
    if( g_mainIsDone ){
      
      if( !g_options.waitForChildren ){
        goto kill_and_reap_children ;
      }
      
      int found = find_children();
      if( found == 0 ){
        goto no_children_remain ;
      }
      
    }
    
    // end of main loop
  }
  
 kill_and_reap_children:
  while(1){
    int found = find_children();
    if( found < 0 ){
      // error accessing proc, try again
      continue;
    }
    
    if( found == 0 ){
      // no children remain
      goto no_children_remain ;
    }
    
    kill_and_reap_children( found );
  }
  
 no_children_remain:
  
  if( g_options.showstats ){
    fprintf( stderr, "children-reaped : %" PRIu64 "\n", g_stats.reaped );
    fprintf( stderr, "children-killed : %" PRIu64 "\n", g_stats.killed );
    fflush( stderr );
  }
  
  return g_mainExit ;
}

static void reap_children( void ){
  int reapedPid ;
  int status    ;
  
  while( reapedPid = waitpid( -1, &status, WNOHANG ) ){
    if( reapedPid < 0 ){
      // nothing more to reap
      break ;
    }
    
    g_stats.reaped ++ ;
    
    if( g_options.verbose ){
      fprintf( stderr, "reaped child : %d\n", reapedPid );
      fflush( stderr );
    }
    
    if( ! g_mainIsDone && reapedPid == g_mainChildPid ){
      g_mainIsDone = 1 ;
      g_mainExit   = WEXITSTATUS( status );
    }
  }
}

static int setup_options( int argc, char ** argv ){
  
  while(1){
    if( ! optind ){ optind = 1; }
    
    static struct option options[] = {
      { "verbose"          , no_argument, 0, 0 },
      { "stats"            , no_argument, 0, 0 },
      { "kill-on-signal"   , no_argument, 0, 0 },
      { "wait-for-children", no_argument, 0, 0 },
      { "all"              , no_argument, 0, 0 },
      { 0                  , 0          , 0 ,0 },
    };
    
    int optionIndex ;
    int cc = getopt_long( argc, argv, "+", options, &optionIndex );
    
    if( cc == -1 ){
      break;
    }
    
    if( cc == 0 ){
      if( optionIndex == 0 ){
        g_options.verbose = 1;
      } else if( optionIndex == 1 ){
        g_options.showstats = 1;
      } else if( optionIndex == 2 ){
        g_options.killOnSignal = 1;
      } else if( optionIndex == 3 ){
        g_options.waitForChildren = 1;
      } else if( optionIndex == 4 ){
        g_options.verbose         = 1 ;
        g_options.showstats       = 1 ;
        g_options.killOnSignal    = 1 ;
        g_options.waitForChildren = 1 ;
      } else {
        fprintf( stderr, "unexpected long option index in argument parsing\n" );
      }
    }
    
    if( cc > 0 ){
      fprintf( stderr, "unexpected error in argument parsing\n" );
      exit(1);
    }
    
  }
  
  if( ! (argc - optind ) ){
    fprintf( stderr, "you must specify the program to run\n" );
    exit(1);
  }
  
  return optind ;
}

static void setup_signal_masks( void ){
  sigemptyset( & g_mask );
  sigaddset( & g_mask, SIGCHLD );
  sigaddset( & g_mask, SIGHUP  );
  sigaddset( & g_mask, SIGINT  );
  sigaddset( & g_mask, SIGQUIT );
  sigaddset( & g_mask, SIGTERM );
  sigaddset( & g_mask, SIGUSR1 );
  sigaddset( & g_mask, SIGUSR2 );
}

static void set_ourself_as_reaper( void ){
  int result = prctl( PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0 );
  if( result != 0 ){
    fprintf( stderr, "could not set process to be subreaper\n" );
    fflush( stderr );
    exit( 1 );
  }
}

static void setup_signal_handlers( void ){
  setup_handler( SIGCHLD, note_signal );
  setup_handler( SIGHUP , note_signal );
  setup_handler( SIGINT , note_signal );
  setup_handler( SIGQUIT, note_signal );
  setup_handler( SIGTERM, note_signal );
  setup_handler( SIGUSR1, note_signal );
  setup_handler( SIGUSR2, note_signal );
}

static void setup_handler( int signum, void(*handler)(int) ){
  struct sigaction sa ;
  sa.sa_handler = handler ;
  sigemptyset( &sa.sa_mask );
  sa.sa_flags = SA_RESTART ;
  if( -1 == sigaction( signum, &sa, NULL ) ){
    fprintf( stderr, "could not set signal(%d) handler : %d : %s\n", signum, errno, strerror( errno ) );
    exit( 1 );
  }
}

static void note_signal( int signum ){
  g_received.signalPending = 1 ;
  
  switch( signum ){
  case SIGCHLD: g_received.sigChld = 1 ; break ;
  case SIGHUP : g_received.sigHup  = 1 ; break ;
  case SIGINT : g_received.sigInt  = 1 ; break ;
  case SIGQUIT: g_received.sigQuit = 1 ; break ;
  case SIGTERM: g_received.sigTerm = 1 ; break ;
  case SIGUSR1: g_received.sigUsr1 = 1 ; break ;
  case SIGUSR2: g_received.sigUsr2 = 1 ; break ;
  default:      g_received.unknown = 1 ; break ;
  };
}

static void wait_on_signal(){
  // note that a signal may come in between handling and calling this
  // in that event we want to return immediately without bothering to
  // actually suspend at all
  // 
  
  sigprocmask( SIG_BLOCK, &g_mask, &g_oldmask );
  
  do {
    
    // now that we are blocking the signals, we check to see if any arrived since last time we waited
    // if so, we return immediately
    if( g_received.signalPending ){
      
      // unset the note of receipt before unblocking signals and returning
      // 
      g_received.signalPending = 0 ;
      
      sigprocmask( SIG_SETMASK, &g_oldmask, NULL );
      return ;
    }
    
    // if not, wait on something to happen
    sigsuspend( &g_oldmask );
    
  } while( 1 );
  
}

static void pass_on_signal( int pid, int signum ){
 attempt_to_send:
  if( -1 == kill( pid, signum ) ){
    int result = errno ;
    
    switch( result ){
      
    case EINTR:
      goto attempt_to_send ;
      
    case EINVAL:
      if( g_options.verbose ){
        fprintf( stderr, "failed to pass on signal : invalid signal number : %d\n", signum );
        fflush( stderr );
        break;
      }
      
    case ESRCH:
      if( g_options.verbose ){
        fprintf( stderr, "failed to pass on signal : child process not found\n" );
        fflush( stderr );
        break;
      }
      
    case EPERM:
      if( g_options.verbose ){
        fprintf( stderr, "failed to pass on signal : permissions error\n" );
        fflush( stderr );
        break ;
      }
      
    default:
      if( g_options.verbose ){
        fprintf( stderr, "failed to pass on signal : bad kill return : %d\n", result );
        fflush( stderr );
        break;
      }
    }
  }
}

static int find_children(){
  
  DIR * dir = opendir( "/proc/" );
  if( ! dir ){
    return -1 ;
  }
  
  struct dirent * entry     ;
  int             index = 0 ;
  while( 1 ){
    entry = readdir( dir );
    if( ! entry ){
      break ;
    }
    
    if( ! isnum( entry->d_name ) ){
      continue ;
    }
    
    if( ! stat_to_child( entry, &g_children[ index ] ) ){
      closedir( dir );
      return -1;
    }
    
    if( g_children[ index ].ppid != g_ourpid ){
      continue ;
    }
    
    index ++ ;
    if( index == PJ_PER_BATCH ){
      break ;
    }
  }
  
  closedir( dir );
  
  return index ;
}

static int stat_to_child( struct dirent * entry, struct Child * child ){
  
  int result ;
  
  char statpath[256];
  result = snprintf( statpath, sizeof( statpath ), "/proc/%s/stat", (char*)&entry->d_name );
  if( result < 0 || result == sizeof( statpath ) ){
    return 0;
  }
  
 try_open_again:;
  int fd = open( statpath, O_RDONLY );
  if( fd == -1 ){
    // if we were just interupted, try again
    // 
    if( errno == EINTR ){ goto try_open_again ; }
    
    // otherwise, abandon this attempt
    // 
    fprintf( stderr, "/proc/<pid>/stat open error\n" );
    return 0;
  }
  
 try_read_again:;
  char statdata[1024];
  result = read( fd, statdata, (sizeof( statdata ) - 1) );
  if( result == -1 ){
    // if interupted, try again
    // 
    if( errno == EINTR ){ goto try_read_again ; }
    
    // otherwise, abandon attempt
    // 
    fprintf( stderr, "/proc/<pid>/stat read error\n" );
    close( fd );
    return 0;
  }
  
  close( fd );
  
  statdata[ result ] = '\0';
  
  // now parse out the data we want
  // pid, ppid and state
  
  result = sscanf(
    statdata
    , "%d %*s %c %d "
    , & child->pid
    , & child->state
    , & child->ppid
  );
  
  if( result == EOF ){
    fprintf( stderr, "sscanf error\n" );
    return 0;
  }
  
  if( result < 3 ){
    fprintf( stderr, "sscanf failed to find all items\n" );
    return 0;
  }
  
  return 1 ;
}

int isnum( char * ss ){
  for( ; *ss ; ss++ ){
    if( ! ( *ss >= '0' && *ss <= '9' ) ){
      return 0;
    }
  }
  return 1;
}

static int kill_and_reap_children( int found ){
  int weNeedToReap = 0 ;
  
  for( int index = 0 ; index < found ; index++ ){
    struct Child * child = &g_children[ index ];
    
    switch( child->state ){
    case 'Z': weNeedToReap = 1                               ; break;
      
    case 'R': kill_child( child, "running"                  ); break;
    case 'S': kill_child( child, "sleeping"                 ); break;
    case 'D': kill_child( child, "uninterruptibly-sleeping" ); break;
    case 'T': kill_child( child, "traced/stopped"           ); break;
    case 'W': kill_child( child, "paging"                   ); break;
      
    default:
      fprintf( stderr, "unknown child state pid:%d ppid:%d state:%c\n", child->pid, child->ppid, child->state );
      fflush( stderr );
    }
  }
  
  if( weNeedToReap ){
    reap_children();
  }
}

void kill_child( struct Child * child, char * description ){
  if( -1 == kill( child->pid, SIGKILL ) ){
    fprintf( stderr, "error killing %s child( %d ) : %d : %s\n"
             , description
             , child->pid
             , errno
             , strerror( errno )
    );
  } else {
    g_stats.killed ++ ;
    if( g_options.verbose ){
      fprintf( stderr, "kill-child pid:%d state:%c\n", child->pid, child->state );
    }
  }
}

int start_main_child( int argc, char ** argv ){
  int childpid = fork();
  
  if( childpid == -1 ){
    fprintf( stderr, "fork failed : %d : %s\n", errno, strerror( errno ) );
    exit(1);
  }
  
  if( childpid != 0 ){
    if( g_options.verbose ){
      fprintf( stderr, "forked-child pid:%d\n", childpid );
    }
    
    return childpid ;
  }
  
  if( -1 == execvp( *argv, argv ) ){
    fprintf( stderr, "exec in fork failed : %d : %s\n", errno, strerror( errno ) );
    exit(1);
  }
  
  // we can't reach this as if execvp doesn't have an error, it will have replaced the process
  //
  fprintf( stderr, "impossible error\n" );
  exit(1);
}
