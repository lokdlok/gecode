/* -*- mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
/*
 *  Main authors:
 *     Christian Schulte <schulte@gecode.org>
 *
 *  Copyright:
 *     Christian Schulte, 2009
 *
 *  Last modified:
 *     $Date$ by $Author$
 *     $Revision$
 *
 *  This file is part of Gecode, the generic constraint
 *  development environment:
 *     http://www.gecode.org
 *
 *  Permission is hereby granted, free of charge, to any person obtaining
 *  a copy of this software and associated documentation files (the
 *  "Software"), to deal in the Software without restriction, including
 *  without limitation the rights to use, copy, modify, merge, publish,
 *  distribute, sublicense, and/or sell copies of the Software, and to
 *  permit persons to whom the Software is furnished to do so, subject to
 *  the following conditions:
 *
 *  The above copyright notice and this permission notice shall be
 *  included in all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 *  LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 *  OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 *  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <gecode/search.hh>
#include <gecode/search/support.hh>
#include <gecode/search/worker.hh>
#include <gecode/search/parallel/path.hh>

#include <gecode/support/thread.hh>

namespace Gecode { namespace Search { namespace Parallel {

  /// Parallel BAB engine
  class BAB : public Engine {
  protected:
    /// Parallel depth-first search worker
    class Worker : public Search::Worker, public Support::Runnable {
    private:
      /// Reference to engine
      BAB& engine;
      /// Mutex for access to worker
      Support::Mutex m;
      /// Current path ins search tree
      Path path;
      /// Current space being explored
      Space* cur;
      /// Distance until next clone
      unsigned int d;
      /// Number of entries not yet constrained to be better
      int mark;
      /// Best solution found so far
      Space* best;
      /// Whether worker is currently idle
      bool idle;
    public:
      /// Initialize for space \a s (of size \a sz) with engine \a e
      Worker(Space* s, size_t sz, BAB& e);
      /// Start execution of worker
      virtual void run(void);
      /// Hand over some work (NULL if no work available)
      Space* steal(unsigned long int& d);
      /// Try to find some work
      void find(void);
      /// Accept better solution \a b
      void better(Space* b);
      /// Return statistics
      Statistics statistics(void);
      /// Destructor
      ~Worker(void);
    };
    /// Search options
    const Options _opt;
    /// Array of worker references
    Worker** _worker;
  public:
    /// Provide access to search options
    const Options& opt(void) const;
    /// Return number of workers
    unsigned int workers(void) const;
    /// Provide access to worker \a i
    Worker* worker(unsigned int i) const;
    
    /// \name Commands from engine to workers and wait management
    //@{
    /// Commands from engine to workers
    enum Cmd {
      C_WORK,     ///< Perform work
      C_WAIT,     ///< Run into wait lock
      C_TERMINATE ///< Terminate
    };
  protected:
    /// The current command
    volatile Cmd _cmd;
    /// Mutex for forcing workers to wait
    Support::Mutex _m_wait;
  public:
    /// Return current command
    Cmd cmd(void) const;
    /// Block all workers
    void block(void);
    /// Release all workers
    void release(Cmd c);
    /// Ensure that worker waits
    void wait(void);
    //@}

    /// \name Termination control
    //@{
  protected:
    /// Mutex for access to termination information
    Support::Mutex _m_terminate;
    /// Number of workers that have not yet acknowledged termination
    volatile unsigned int _n_not_acknowledged;
    /// Event for termination acknowledgment
    Support::Event _e_acknowledged;
    /// Mutex for waiting for termination
    Support::Mutex _m_wait_terminate;
    /// Number of not yet terminated workers
    volatile unsigned int _n_not_terminated;
    /// Event for termination (all threads have terminated)
    Support::Event _e_terminate;
  public:
    /// For worker to acknowledge termination command
    void acknowledge(void);
    /// For worker to register termination
    void terminated(void);
    /// For worker to wait until termination is legal
    void wait_terminate(void);
    //@}

    /// \name Search control
    //@{
  protected:
    /// Mutex for search
    Support::Mutex m_search;
    /// Event for search (solution found, no more solutions, search stopped)
    Support::Event e_search;
    /// Queue of solutions
    Support::DynamicQueue<Space*,Heap> solutions;
    /// Best solution so far
    Space* best;
    /// Number of busy workers
    volatile unsigned int n_busy;
    /// Whether a worker had been stopped
    volatile bool has_stopped;
    /// Whether search state changed such that signal is needed
    bool signal(void) const;
  public:
    /// Report solution \a s
    void solution(Space* s);
    /// Report that worker is idle
    void idle(void);
    /// Report that worker is busy
    void busy(void);
    /// Report that worker has been stopped
    void stop(void);
    //@}

    /// \name Engine interface
    //@{
    /// Initialize for space \a s (of size \a sz) with options \a o
    BAB(Space* s, size_t sz, const Options& o);
    /// Return next solution (NULL, if none exists or search has been stopped)
    virtual Space* next(void);
    /// Return statistics
    virtual Statistics statistics(void) const;
    /// Check whether engine has been stopped
    virtual bool stopped(void) const;
    /// Destructor
    virtual ~BAB(void);
    //@}
  };


  /*
   * Engine: basic access routines
   */
  forceinline const Options&
  BAB::opt(void) const {
    return _opt;
  }
  forceinline unsigned int
  BAB::workers(void) const {
    return opt().threads;
  }
  forceinline BAB::Worker*
  BAB::worker(unsigned int i) const {
    return _worker[i];
  }


  /*
   * Engine: command and wait handling
   */
  forceinline BAB::Cmd
  BAB::cmd(void) const {
    return _cmd;
  }
  forceinline void 
  BAB::block(void) {
    _cmd = C_WAIT;
    _m_wait.acquire();
  }
  forceinline void 
  BAB::release(Cmd c) {
    _cmd = c;
    _m_wait.release();
  }
  forceinline void 
  BAB::wait(void) {
    _m_wait.acquire(); _m_wait.release();
  }


  /*
   * Engine: initialization
   */
  forceinline
  BAB::Worker::Worker(Space* s, size_t sz, BAB& e)
    : Search::Worker(sz), engine(e), d(0), mark(0), best(NULL), idle(false) {
    if (s != NULL) {
      cur = (s->status(*this) == SS_FAILED) ? NULL : snapshot(s,engine.opt());
      if (cur == NULL)
        fail++;
    } else {
      cur = NULL;
    }
    current(s);
    current(NULL);
    current(cur);
  }

  forceinline
  BAB::BAB(Space* s, size_t sz, const Options& o)
    : _opt(o), solutions(heap), best(NULL) {
    // Create workers
    _worker = static_cast<Worker**>
      (heap.ralloc(workers() * sizeof(Worker*)));
    // The first worker gets the entire search tree
    _worker[0] = new Worker(s,sz,*this);
    // All other workers start with no work
    for (unsigned int i=1; i<workers(); i++)
      _worker[i] = new Worker(NULL,sz,*this);
    // Block all workers
    block();
    // Create and start threads
    for (unsigned int i=0; i<workers(); i++) {
      Support::Thread t(_worker[i]);
    }
    // Initialize termination information
    _n_not_acknowledged = workers();
    _n_not_terminated = workers();
    // Initialize search information
    n_busy = workers();
    has_stopped = false;
  }


  /*
   * Engine: search control
   */
  Space* 
  BAB::next(void) {
    // Invariant: the worker holds the wait mutex
    m_search.acquire();
    if (!solutions.empty()) {
      // No search to be done, take leftover solution
      Space* s = solutions.pop();
      m_search.release();
      return s;
    }
    // No more solutions?
    if (n_busy == 0) {
      m_search.release();
      return NULL;
    }
    // We ignore stopped (that's okay because it will be reported again then)
    has_stopped = false;
    m_search.release();
    // Okay, now search has to continue, make the guys work
    release(C_WORK);

    /*
     * Wait until a search related event has happened. It might be that
     * the event has already been signalled in the last run, but the
     * solution has been removed. So we have to try until there has
     * something new happened.
     */
    while (true) {
      e_search.wait();
      m_search.acquire();
      if (!solutions.empty()) {
        // Report solution
        Space* s = solutions.pop();
        m_search.release();
        // Make workers wait again
        block();
        return s;
      }
      // No more solutions or stopped?
      if ((n_busy == 0) || has_stopped) {
        m_search.release();
        // Make workers wait again
        block();
        return NULL;
      }
      m_search.release();
    }
    GECODE_NEVER;
    return NULL;
  }


  bool 
  BAB::stopped(void) const {
    return has_stopped;
  }




  /*
   * Statistics
   */
  forceinline Statistics
  BAB::Worker::statistics(void) {
    m.acquire();
    Statistics s = *this;
    s.memory += path.size();
    m.release();
    return s;
  }

  Statistics 
  BAB::statistics(void) const {
    Statistics s;
    for (unsigned int i=0; i<workers(); i++)
      s += worker(i)->statistics();
    return s;
  }


  /*
   * Engine: search control
   */
  forceinline bool
  BAB::signal(void) const {
    return solutions.empty() && (n_busy > 0) && !has_stopped;
  }
  void
  BAB::Worker::better(Space* b) {
    m.acquire();
    delete best;
    best = b->clone(false);
    mark = path.entries();
    if (cur != NULL)
      cur->constrain(*best);
    m.release();
  }
  forceinline void 
  BAB::solution(Space* s) {
    m_search.acquire();
    if (best != NULL) {
      s->constrain(*best);
      if (s->status() == SS_FAILED) {
        delete s;
        m_search.release();
        return;
      } else {
        delete best;
        best = s->clone();
      }
    } else {
      best = s->clone();
    }
    // Announce better solutions
    for (unsigned int i=0; i<workers(); i++)
      worker(i)->better(best);
    bool bs = signal();
    solutions.push(s);
    if (bs)
      e_search.signal();
    m_search.release();
  }
  forceinline void 
  BAB::idle(void) {
    m_search.acquire();
    bool bs = signal();
    n_busy--;
    if (bs && (n_busy == 0))
      e_search.signal();
    m_search.release();
  }
  forceinline void 
  BAB::busy(void) {
    m_search.acquire();
    assert(n_busy > 0);
    n_busy++;
    m_search.release();
  }
  forceinline void 
  BAB::stop(void) {
    m_search.acquire();
    bool bs = signal();
    has_stopped = true;
    if (bs)
      e_search.signal();
    m_search.release();
  }
  

  /*
   * Engine: termination control
   */
  forceinline void 
  BAB::terminated(void) {
    unsigned int n;
    _m_terminate.acquire();
    n = --_n_not_terminated;
    _m_terminate.release();
    // The signal must be outside of the look, otherwise a thread might be 
    // terminated that still holds a mutex.
    if (n == 0)
      _e_terminate.signal();
  }

  forceinline void 
  BAB::acknowledge(void) {
    _m_terminate.acquire();
    if (--_n_not_acknowledged == 0)
      _e_acknowledged.signal();
    _m_terminate.release();
  }

  forceinline void 
  BAB::wait_terminate(void) {
    _m_wait_terminate.acquire();
    _m_wait_terminate.release();
  }


  /*
   * Worker: finding and stealing working
   */
  forceinline Space*
  BAB::Worker::steal(unsigned long int& d) {
    /*
     * Make a quick check whether the worker might have work
     *
     * If that is not true any longer, the worker will be asked
     * again eventually.
     */
    if (!path.steal())
      return NULL;
    m.acquire();
    Space* s = path.steal(*this,d);
    m.release();
    // Tell that there will be one more busy worker
    if (s != NULL) 
      engine.busy();
    return s;
  }

  forceinline void
  BAB::Worker::find(void) {
    // Try to find new work (even if there is none)
    for (unsigned int i=0; i<engine.workers(); i++) {
      unsigned long int r_d;
      if (Space* s = engine.worker(i)->steal(r_d)) {
        // Reset this guy
        m.acquire();
        idle = false;
        d = 0;
        cur = s;
        mark = 0;
        if (best != NULL)
          cur->constrain(*best);
        Search::Worker::reset(cur,r_d);
        m.release();
        return;
      }
    }
  }

  void
  BAB::Worker::run(void) {
    // Okay, we are in business, start working
    while (true) {
      switch (engine.cmd()) {
      case BAB::C_WAIT:
        // Wait
        engine.wait();
        break;
      case BAB::C_TERMINATE:
        // Acknowledge termination request
        engine.acknowledge();
        // Wait until termination can proceed
        engine.wait_terminate();
        // Terminate thread
        engine.terminated();
        return;
      case BAB::C_WORK:
        // Perform exploration work
        {
          m.acquire();
          if (idle) {
            m.release();
            // Try to find new work
            find();
          } else if (cur != NULL) {
            start();
            if (stop(engine.opt().stop,path.size())) {
              // Report stop
              m.release();
              engine.stop();
            } else {
              /*
               * The invariant maintained by the engine is:
               *   For all nodes stored at a depth less than mark, there
               *   is no guarantee of betterness. For those above the mark,
               *   betterness is guaranteed.
               */
              node++;
              switch (cur->status(*this)) {
              case SS_FAILED:
                fail++;
                delete cur;
                cur = NULL;
                Worker::current(NULL);
                m.release();
                break;
              case SS_SOLVED:
                {
                  // Deletes all pending branchings
                  (void) cur->description();
                  Space* s = cur->clone(false);
                  delete cur;
                  cur = NULL;
                  Worker::current(NULL);
                  m.release();
                  engine.solution(s);
                }
                break;
              case SS_BRANCH:
                {
                  Space* c;
                  if ((d == 0) || (d >= engine.opt().c_d)) {
                    c = cur->clone();
                    d = 1;
                  } else {
                    c = NULL;
                    d++;
                  }
                  const BranchingDesc* desc = path.push(*this,cur,c);
                  Worker::push(c,desc);
                  cur->commit(*desc,0);
                  m.release();
                }
                break;
              default:
                GECODE_NEVER;
              }
            }
          } else if (path.next(*this)) {
            cur = path.recompute(d,engine.opt().a_d,*this,best,mark);
            Worker::current(cur);
            m.release();
          } else {
            idle = true;
            m.release();
            // Report that worker is idle
            engine.idle();
          }
        }
        break;
      default:
        GECODE_NEVER;
      }
    }
  }


  /*
   * Termination and deletion
   */
  BAB::Worker::~Worker(void) {
    delete cur;
    delete best;
    path.reset();
  }

  BAB::~BAB(void) {
    // Grab the wait mutex for termination
    _m_wait_terminate.acquire();
    // Release all threads
    release(C_TERMINATE);
    // Wait until all threads have acknowledged termination request
    _e_acknowledged.wait();
    // Release waiting threads
    _m_wait_terminate.release();    
    // Wait until all threads have in fact terminated
    _e_terminate.wait();
    // Now all threads are terminated!
    heap.rfree(_worker);
  }


  // Create parallel depth-first engine
  Engine* bab(Space* s, size_t sz, const Options& o) {
    return new BAB(s,sz,o);
  }

}}}

// STATISTICS: search-any