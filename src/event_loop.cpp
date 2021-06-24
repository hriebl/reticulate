//
// This code implements the ability to poll for events while Python code
// is executing. This is primarily used to ensure that R_ProcessEvents()
// is invoked even while the Python interpreter is running in the foreground.
//
// Since execution of Python code occurs within native C++ code the normal R
// interpreter event polling that occurs during do_eval does not have
// a chance to execute. Typically within Rcpp packages long running user code
// will call Rcpp::checkForInterrupts periodically to check for an interrupt
// and exit via an exception if there is one (this call to checkForInterrupts
// calls the R process events machinery as well). However there is no opportunity
// to do this during Python execution, so we need to find a way to get periodic
// callbacks during the Python interpreter's processing to perform this check.
//
// This is provided for via the Py_AddPendingCall function, which enables the
// scheduling of a C callback on the main interpreter thread *during* the
// execution of Python code. Unfortunately this call occurs very eagerly, so we
// can't just schedule a callback and have the callback reschedule itself (this
// completely swamps the interpreter). Rather, we need to have a background
// thread that will perform the Py_AddPendingCall on a throttled basis (both
// time wise and in terms of only scheduling an additional callback while the
// Python interpreter remains running).
//

#include "event_loop.h"

#include "reticulate.h"

#include "libpython.h"
using namespace libpython;

#include "signals.h"
#include "tinythread.h"

#include <R_ext/Boolean.h>

#ifdef _WIN32
# define LibExtern __declspec(dllimport) extern
#else
# define LibExtern extern
#endif

extern "C" {
extern void R_ProcessEvents();
extern Rboolean R_ToplevelExec(void (*func)(void*), void*);
}

namespace reticulate {
namespace event_loop {

namespace {

// Class that is used to signal the need to poll for events between
// threads. The function called by the Python interpreter during execution
// (pollForEvents) always calls requestPolling to keep polling alive. The
// background thread periodically attempts to "collect" this request and if
// successful re-schedules the pollForEvents function using
// Py_AddPendingCall. This allows us to prevent the background thread from
// continually scheduling pollForEvents even when the Python interpreter is
// not running (because once pollForEvents is no longer being called by the
// Python interpreter no additional calls to pollForEvents will be
// scheduled)
class EventPollingSignal {
public:
  EventPollingSignal() : pollingRequested_(true) {}

  void requestPolling() {
    tthread::lock_guard<tthread::mutex> lock(mutex_);
    pollingRequested_ = true;
  }

  bool collectRequest() {
    tthread::lock_guard<tthread::mutex> lock(mutex_);
    bool requested = pollingRequested_;
    pollingRequested_ = false;
    return requested;
  }

private:
  EventPollingSignal(const EventPollingSignal& other);
  EventPollingSignal& operator=(const EventPollingSignal&);
private:
  tthread::mutex mutex_;
  bool pollingRequested_;
};

EventPollingSignal s_pollingSignal;

// Forward declarations
int pollForEvents(void*);

// Background thread which re-schedules pollForEvents on the main Python
// interpreter thread every 250ms so long as the Python interpeter is still
// running (when it stops running it will stop calling pollForEvents and
// the polling signal will not be set).
void eventPollingWorker(void *) {
  
  while (true) {

    // Throttle via sleep
    tthread::this_thread::sleep_for(tthread::chrono::milliseconds(250));

    // Schedule polling on the main thread if the interpeter is still running
    // Note that Py_AddPendingCall is documented to be callable from a background
    // thread: "This function doesn’t need a current thread state to run, and it
    // doesn’t need the global interpreter lock."
    // (see: https://docs.python.org/3/c-api/init.html#c.Py_AddPendingCall)
    if (s_pollingSignal.collectRequest())
      Py_AddPendingCall(pollForEvents, NULL);

  }
  
}

void processEvents(void* data) {
  R_ProcessEvents();
}

// Callback function scheduled to run on the main Python interpreter loop. This
// is scheduled using Py_AddPendingCall, which ensures that it is run on the
// main thread while the interpreter is executing. Note that we can't just have
// this function keep re-scheduling itself or the interpreter would be swamped
// with just calling and re-calling this function. Rather, we need to throttle
// the scheduling of the function by using a background thread + a sleep timer.
int pollForEvents(void*) {

  DBG("Polling for events.\n");
  
  // Process events. We wrap this in R_ToplevelExec just to avoid jumps.
  // Suspend interrupts here so we don't inadvertently handle them.
  reticulate::signals::InterruptsSuspendedScope scope;
  R_ToplevelExec(processEvents, NULL);
  
  // Request that the background thread schedule us to be called again
  // (this is delegated to a background thread so that these requests
  // can be throttled)
  s_pollingSignal.requestPolling();
  
  // Success!
  return 0;
  
}

} // anonymous namespace

// Initialize event loop polling background thread
void initialize() {
  tthread::thread t(eventPollingWorker, NULL);
  t.detach();
}

} // namespace event_loop
} // namespace reticulate

