#ifndef RBX_BUILTIN_THREAD_HPP
#define RBX_BUILTIN_THREAD_HPP

#include "builtin/object.hpp"

#define THREAD_STACK_SIZE 4194304

namespace rubinius {

  class Channel;
  class Exception;
  class LookupTable;
  class Randomizer;
  class Array;

  /**
   *  Ruby Thread implementation.
   *
   *  Each Thread is backed by a native thread. This class
   *  provides the interface Ruby expects to see to manipulate
   *  Thread execution.
   */
  class Thread : public Object {
    /** Thread is created and valid and not yet done? */
    Object* alive_;        // slot

    /** Thread is currently sleeping and not running? */
    Object* sleep_;        // slot

    Channel* control_channel_; // slot

    /** LookupTable of objects that contain themselves. */
    LookupTable* recursive_objects_;  // slot

    Thread* debugger_thread_; // slot

    Fixnum* thread_id_; // slot

    Randomizer* randomizer_; // slot

    LookupTable* locals_; // slot

    Object* group_; // slot
    Object* result_; // slot
    Exception* exception_; // slot
    Object* critical_; // slot
    Object* killed_; // slot
    Fixnum* priority_; // slot
    Fixnum* pid_; // slot

    utilities::thread::SpinLock init_lock_;
    utilities::thread::Mutex join_lock_;
    utilities::thread::Condition join_cond_;

    /// The VM state for this thread and this thread alone
    VM* vm_;

    typedef Object* (*ThreadFunction)(STATE);

    ThreadFunction function_;

  public:
    const static object_type type = ThreadType;

    static void   init(State* state);

  public:
    attr_accessor(alive, Object);

    attr_accessor(sleep, Object);

    attr_accessor(control_channel, Channel);

    attr_accessor(recursive_objects, LookupTable);

    attr_accessor(debugger_thread, Thread);

    attr_accessor(thread_id, Fixnum);

    attr_accessor(randomizer, Randomizer);

    attr_accessor(locals, LookupTable);

    attr_accessor(group, Object);
    attr_accessor(result, Object);
    attr_accessor(exception, Exception);
    attr_accessor(critical, Object);
    attr_accessor(killed, Object);
    attr_accessor(priority, Fixnum);
    attr_accessor(pid, Fixnum);

    VM* vm() const {
      return vm_;
    }

  public:

    /**
     *  Allocate a Thread object.
     *
     *  Object is in a valid but not running state.
     *  It still assumes that #initialize will be
     *  called to fully set it up. The object is
     *  not yet associated with an actual native
     *  thread.
     *
     *  This method also creates a new VM object
     *  to represent its state.
     *
     *  @see  Thread::fork()
     *  @see  Thread::create()
     *
     *  @see  vm/vm.hpp
     *  @see  kernel/thread.rb
     */
    // Rubinius.primitive :thread_allocate
    static Thread* allocate(STATE, Object* self);

    /**
     *  Returns the Thread object for the state.
     *
     *  This is the currently executing Thread.
     */
    // Rubinius.primitive+ :thread_current
    static Thread* current(STATE);

    /**
     *  Attempt to schedule some other Thread.
     */
    // Rubinius.primitive+ :thread_pass
    static Object* pass(STATE, CallFrame* calling_environment);

    /**
     *   List all live threads.
     */
    // Rubinius.primitive :thread_list
    static Array* list(STATE);

  public:   /* Instance primitives */

    /**
     *  Execute the Thread.
     *
     *  Actually creates the native thread and starts it.
     *  The native thread will start executing this Thread's
     *  #__run__ method.
     *
     *  @see  Thread::allocate()
     *
     *  @see  kernel/thread.rb
     */
    // Rubinius.primitive :thread_fork
    Object* fork(STATE);

    /**
     *  Execute the Thread.
     *
     *  This leaves the thread in an attached state, so that
     *  a pthread_join() later on will work.
     */
    int fork_attached(STATE);

    /**
     *  Retrieve the priority set for this Thread.
     *
     *  The value is numeric, higher being more important
     *  but otherwise *potentially* platform-specific for
     *  any other connotations.
     */
    // Rubinius.primitive+ :thread_get_priority
    Object* get_priority(STATE);

    /**
     *  Process an exception raised for this Thread.
     */
    // Rubinius.primitive :thread_raise
    Object* raise(STATE, GCToken gct, Exception* exc, CallFrame* calling_environment);

    // Rubinius.primitive :thread_set_exception
    Object* set_exception(STATE, Exception* exc);

    /**
     *  Returns current exception
     */
    // Rubinius.primitive :thread_current_exception
    Object* current_exception(STATE);

    /**
     *  Kill this Thread.
     */
    // Rubinius.primitive :thread_kill
    Object* kill(STATE, GCToken gct, CallFrame* calling_environment);

    /**
     *  Set the priority for this Thread.
     *
     *  The value is numeric, higher being more important
     *  but otherwise *potentially* platform-specific for
     *  any other connotations.
     */
    // Rubinius.primitive :thread_set_priority
    Object* set_priority(STATE, Fixnum* priority);

    /**
     *  Schedule Thread to be run.
     *
     *  This wakes up a sleeping Thread, although it can also
     *  be invoked on an already-running Thread. The Thread
     *  is queued to be run, although not necessarily immediately.
     */
    // Rubinius.primitive :thread_wakeup
    Thread* wakeup(STATE, GCToken gct, CallFrame* calling_environment);

    // Rubinius.primitive :thread_context
    Tuple* context(STATE);

    // Rubinius.primitive :thread_mri_backtrace
    Array* mri_backtrace(STATE, GCToken gct, CallFrame* calling_environment);

    // Rubinius.primitive :thread_join
    Thread* join(STATE, GCToken gct, Object* timeout, CallFrame* calling_environment);

    // Rubinius.primitive :thread_unlock_locks
    Object* unlock_locks(STATE, GCToken gct, CallFrame* calling_environment);

    // This method must only be called after fork() with only one active
    // thread.
    void unlock_after_fork(STATE, GCToken gct);

    /**
     * Retrieve a value store in the thread locals.
     * This is done in a primitive because it also has
     * to consider any running fibers.
     */
    // Rubinius.primitive+ :thread_locals_aref
    Object* locals_aref(STATE, Symbol* key);

    /**
     * Store a value in the thread locals.
     * This is done in a primitive because it also has
     * to consider any running fibers.
     */
    // Rubinius.primitive :thread_locals_store
    Object* locals_store(STATE, Symbol* key, Object* value);

    /**
     * Remove a value from the thread locals.
     * This is done in a primitive because it also has
     * to consider any running fibers.
     */
    // Rubinius.primitive :thread_locals_remove
    Object* locals_remove(STATE, Symbol* key);

    /**
     * Retrieve the keys for all thread locals.
     * This is done in a primitive because it also has
     * to consider any running fibers.
     */
    // Rubinius.primitive :thread_locals_keys
    Array* locals_keys(STATE);

    /**
     * Check whether a given key has a value store in the thread locals.
     * This is done in a primitive because it also has
     * to consider any running fibers.
     */
    // Rubinius.primitive+ :thread_locals_has_key
    Object* locals_has_key(STATE, Symbol* key);

    void init_lock();
    void stopped();

    /**
     *  Create a Thread object.
     *
     *  Used by the Thread::allocate() primitive, creates
     *  the Thread object and associates it with the provided
     *  VM state object. The Thread is not yet associated
     *  with a native thread.
     *
     *  @see  Thread::allocate().
     */
    static Thread* create(STATE, VM* vm);
    static Thread* create(STATE, VM* vm, ThreadFunction function);
    static Thread* create(STATE, Object* self, ThreadFunction function);
    static Thread* create(STATE, Object* self, VM* vm, ThreadFunction function);
    static Thread* create(STATE, Class* klass, VM* vm);

    static void finalize(STATE, Thread* thread);

    int start_thread(STATE, void* (*function)(void*));
    static void* run(void*);

    static Object* main_thread(STATE);

  public:   /* TypeInfo */

    class Info : public TypeInfo {
    public:
      BASIC_TYPEINFO(TypeInfo)
    };
  };
}


#endif
