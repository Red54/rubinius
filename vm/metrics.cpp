#include "vm.hpp"
#include "metrics.hpp"

#include "environment.hpp"
#include "object_utils.hpp"
#include "shared_state.hpp"
#include "configuration.hpp"
#include "ontology.hpp"
#include "system_diagnostics.hpp"

#include "builtin/class.hpp"
#include "builtin/fixnum.hpp"
#include "builtin/integer.hpp"
#include "builtin/lookup_table.hpp"
#include "builtin/thread.hpp"
#include "builtin/tuple.hpp"

#include "jit/llvm/state.hpp"

#include "gc/managed.hpp"

#include "dtrace/dtrace.h"

#include "util/logger.hpp"

#include <fcntl.h>
#include <stdint.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/utsname.h>

#include <ostream>

namespace rubinius {
  using namespace utilities;

  namespace metrics {
    FileEmitter::FileEmitter(STATE, MetricsMap& map, std::string path)
      : MetricsEmitter()
      , metrics_map_(map)
      , path_(path)
      , fd_(-1)
    {
      // TODO: Make this a proper feature of the config facility.
      state->shared().env()->expand_config_value(
          path_, "$PID", state->shared().pid.c_str());

      initialize();
    }

    FileEmitter::~FileEmitter() {
      cleanup();
    }

#define RBX_METRICS_FILE_BUFLEN   256

    void FileEmitter::send_metrics() {
      char buf[RBX_METRICS_FILE_BUFLEN];

      for(MetricsMap::iterator i = metrics_map_.begin();
          i != metrics_map_.end();
          ++i)
      {
        snprintf(buf, RBX_METRICS_FILE_BUFLEN, "%s%lld",
            i == metrics_map_.begin() ? "" : " ", (long long unsigned int)(*i)->second);
        if(write(fd_, buf, strlen(buf)) < 0) {
          logger::error("%s: unable to write file metrics", strerror(errno));
        }
      }

      if(write(fd_, "\n", 1)) {
        logger::error("%s: unable to write file metrics", strerror(errno));
      }
    }

    void FileEmitter::initialize() {
      if(!(fd_ = ::open(path_.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0660))) {
        logger::error("%s: unable to open metrics file", strerror(errno));
      }

      if(lseek(fd_, 0, SEEK_END) == 0) {
        char buf[RBX_METRICS_FILE_BUFLEN];

        for(MetricsMap::iterator i = metrics_map_.begin();
            i != metrics_map_.end();
            ++i)
        {
          snprintf(buf, RBX_METRICS_FILE_BUFLEN, "%s%s",
              i == metrics_map_.begin() ? "" : ", ", (*i)->first.c_str());
          if(write(fd_, buf, strlen(buf)) < 0) {
            logger::error("%s: unable to write file metrics", strerror(errno));
          }
        }

        if(write(fd_, "\n", 1)) {
          logger::error("%s: unable to write file metrics", strerror(errno));
        }
      }
    }

    void FileEmitter::cleanup() {
      if(fd_ > 0) {
        close(fd_);
        fd_ = -1;
      }
    }

    void FileEmitter::reinit() {
      // Don't turn on FileEmitter in children by default.
      cleanup();
    }

    StatsDEmitter::StatsDEmitter(MetricsMap& map, std::string server, std::string prefix)
      : MetricsEmitter()
      , metrics_map_(map)
      , socket_fd_(-1)
    {
      size_t colon = server.find(':');
      if(colon == std::string::npos) {
        host_ = "localhost";
        if(server.size() > 0) {
          port_ = server;
        } else {
          port_ = "8125";
        }
      } else {
        host_ = server.substr(0, colon);
        port_ = server.substr(colon + 1);
      }

      size_t index = prefix.find("$nodename");
      if(index != std::string::npos) {
        struct utsname name;

        if(uname(&name)) {
          prefix_ = "";
        } else {
          std::ostringstream parts;
          std::string n = name.nodename;

          for(size_t p = n.size(), i = p; i != 0; p = i - 1) {
            if((i = n.rfind('.', p)) == std::string::npos) {
              parts << n.substr(0, p + 1);
              break;
            } else {
              parts << n.substr(i + 1, p - i) << ".";
            }

          }

          prefix_ = prefix;
          prefix_.replace(index, strlen("$nodename"), parts.str());
        }
      } else {
        prefix_ = prefix;
      }

      index = prefix_.find("$pid");
      if(index != std::string::npos) {
        std::ostringstream pid;
        pid << getpid();

        prefix_.replace(index, strlen("$pid"), pid.str());
      }

      if(prefix_.size() > 0) prefix_ += ".";

      initialize();
    }

    StatsDEmitter::~StatsDEmitter() {
      cleanup();
    }

#define RBX_METRICS_STATSD_BUFLEN   256

    void StatsDEmitter::send_metrics() {
      char buf[RBX_METRICS_STATSD_BUFLEN];

      for(MetricsMap::iterator i = metrics_map_.begin();
          i != metrics_map_.end();
          ++i)
      {
        snprintf(buf, RBX_METRICS_STATSD_BUFLEN, "%s%s:%lld|g",
            prefix_.c_str(), (*i)->first.c_str(), (long long unsigned int)(*i)->second);
        if(send(socket_fd_, buf, strlen(buf), 0) < 0) {
          logger::error("%s: unable to send StatsD metrics", strerror(errno));
        }
      }
    }

    void StatsDEmitter::initialize() {
      struct addrinfo hints, *res, *res0;

      memset(&hints, 0, sizeof(hints));
      hints.ai_family = PF_UNSPEC;
      hints.ai_socktype = SOCK_DGRAM;

      if(int error = getaddrinfo(host_.c_str(), port_.c_str(), &hints, &res0) < 0) {
        logger::error("%s: unable to get StatsD server address info",
            gai_strerror(error));
        return;
      }

      for(res = res0; res; res = res->ai_next) {
        if((socket_fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) < 0) {
          continue;
        }

        if(connect(socket_fd_, res->ai_addr, res->ai_addrlen) < 0) {
          close(socket_fd_);
          socket_fd_ = -1;
          continue;
        }

        break;
      }

      freeaddrinfo(res0);

      if(socket_fd_ < 0) {
        logger::error("unable to create StatsD UDP socket");
        return;
      }

      fcntl(socket_fd_, F_SETFL, O_NONBLOCK);
    }

    void StatsDEmitter::cleanup() {
      if(socket_fd_ > 0) close(socket_fd_);
    }

    void StatsDEmitter::reinit() {
      cleanup();
      initialize();
    }

    Metrics::Metrics(STATE)
      : InternalThread(state, "rbx.metrics", InternalThread::eSmall)
      , enabled_(true)
      , values_(state)
      , interval_(state->shared().config.system_metrics_interval)
      , timer_(NULL)
      , metrics_lock_()
      , metrics_data_()
      , metrics_history_()
      , metrics_map_()
      , emitter_(NULL)
    {
      map_metrics();

      if(!state->shared().config.system_metrics_target.value.compare("statsd")) {
        emitter_ = new StatsDEmitter(metrics_map_,
            state->shared().config.system_metrics_statsd_server.value,
            state->shared().config.system_metrics_statsd_prefix.value);
      } else if(state->shared().config.system_metrics_target.value.compare("none")) {
        emitter_ = new FileEmitter(state, metrics_map_,
            state->shared().config.system_metrics_target.value);
      }
    }

    Metrics::~Metrics() {
      if(timer_) delete timer_;
      if(emitter_) delete emitter_;

      for(MetricsMap::iterator i = metrics_map_.begin();
          i != metrics_map_.end();
          ++i)
      {
        delete (*i);
      }
    }

    void Metrics::map_metrics() {
      // CodeDB metrics
      metrics_map_.push_back(new MetricsItem(
            "codedb.load.us", metrics_data_.codedb.load_us));

      // Console metrics
      metrics_map_.push_back(new MetricsItem(
            "console.requests.received", metrics_data_.console.requests_received));
      metrics_map_.push_back(new MetricsItem(
            "console.responses.sent", metrics_data_.console.responses_sent));

      // GC metrics
      metrics_map_.push_back(new MetricsItem(
            "gc.young.count", metrics_data_.gc.young_count));
      metrics_map_.push_back(new MetricsItem(
            "gc.young.ms", metrics_data_.gc.young_ms));
      metrics_map_.push_back(new MetricsItem(
            "gc.immix.count", metrics_data_.gc.immix_count));
      metrics_map_.push_back(new MetricsItem(
            "gc.immix.stop.ms", metrics_data_.gc.immix_stop_ms));
      metrics_map_.push_back(new MetricsItem(
            "gc.immix.concurrent.ms", metrics_data_.gc.immix_concurrent_ms));
      metrics_map_.push_back(new MetricsItem(
            "gc.immix.diagnostics.us", metrics_data_.gc.immix_diagnostics_us));
      metrics_map_.push_back(new MetricsItem(
            "gc.large.count", metrics_data_.gc.large_count));
      metrics_map_.push_back(new MetricsItem(
            "gc.large.sweep.us", metrics_data_.gc.large_sweep_us));

      // JIT metrics
      metrics_map_.push_back(new MetricsItem(
            "jit.methods.queued", metrics_data_.jit.methods_queued));
      metrics_map_.push_back(new MetricsItem(
            "jit.methods.compiled", metrics_data_.jit.methods_compiled));
      metrics_map_.push_back(new MetricsItem(
            "jit.methods.failed", metrics_data_.jit.methods_failed));
      metrics_map_.push_back(new MetricsItem(
            "jit.compile_time.us", metrics_data_.jit.compile_time_us));
      metrics_map_.push_back(new MetricsItem(
            "jit.uncommon_exits", metrics_data_.jit.uncommon_exits));
      metrics_map_.push_back(new MetricsItem(
            "jit.inlined.accessors", metrics_data_.jit.inlined_accessors));
      metrics_map_.push_back(new MetricsItem(
            "jit.inlined.methods", metrics_data_.jit.inlined_methods));
      metrics_map_.push_back(new MetricsItem(
            "jit.inlined.blocks", metrics_data_.jit.inlined_blocks));
      metrics_map_.push_back(new MetricsItem(
            "jit.inlined.primitives", metrics_data_.jit.inlined_primitives));
      metrics_map_.push_back(new MetricsItem(
            "jit.inlined.ffi", metrics_data_.jit.inlined_ffi));

      // Lock metrics
      metrics_map_.push_back(new MetricsItem(
            "lock.stop_the_world.ns", metrics_data_.lock.stop_the_world_ns));

      // Machine metrics
      metrics_map_.push_back(new MetricsItem(
            "machine.inline_cache.resets", metrics_data_.machine.inline_cache_resets));
      metrics_map_.push_back(new MetricsItem(
            "machine.methods.invoked", metrics_data_.machine.methods_invoked));
      metrics_map_.push_back(new MetricsItem(
            "machine.blocks.invoked", metrics_data_.machine.blocks_invoked));

      // Memory metrics
      metrics_map_.push_back(new MetricsItem(
            "memory.young.bytes", metrics_data_.memory.young_bytes));
      metrics_map_.push_back(new MetricsItem(
            "memory.young.objects", metrics_data_.memory.young_objects));
      metrics_map_.push_back(new MetricsItem(
            "memory.immix.bytes", metrics_data_.memory.immix_bytes));
      metrics_map_.push_back(new MetricsItem(
            "memory.immix.objects", metrics_data_.memory.immix_objects));
      metrics_map_.push_back(new MetricsItem(
            "memory.immix.chunks", metrics_data_.memory.immix_chunks));
      metrics_map_.push_back(new MetricsItem(
            "memory.large.bytes", metrics_data_.memory.large_bytes));
      metrics_map_.push_back(new MetricsItem(
            "memory.large.objects", metrics_data_.memory.large_objects));
      metrics_map_.push_back(new MetricsItem(
            "memory.symbols", metrics_data_.memory.symbols));
      metrics_map_.push_back(new MetricsItem(
            "memory.symbols.bytes", metrics_data_.memory.symbols_bytes));
      metrics_map_.push_back(new MetricsItem(
            "memory.code.bytes", metrics_data_.memory.code_bytes));
      metrics_map_.push_back(new MetricsItem(
            "memory.jit.bytes", metrics_data_.memory.jit_bytes));
      metrics_map_.push_back(new MetricsItem(
            "memory.promoted.bytes", metrics_data_.memory.promoted_bytes));
      metrics_map_.push_back(new MetricsItem(
            "memory.promoted.objects", metrics_data_.memory.promoted_objects));
      metrics_map_.push_back(new MetricsItem(
            "memory.slab.refills", metrics_data_.memory.slab_refills));
      metrics_map_.push_back(new MetricsItem(
            "memory.slab.refills.fails", metrics_data_.memory.slab_refills_fails));
      metrics_map_.push_back(new MetricsItem(
            "memory.data_objects", metrics_data_.memory.data_objects));
      metrics_map_.push_back(new MetricsItem(
            "memory.capi_handles", metrics_data_.memory.capi_handles));
      metrics_map_.push_back(new MetricsItem(
            "memory.inflated_headers", metrics_data_.memory.inflated_headers));

      // System metrics
      metrics_map_.push_back(new MetricsItem(
            "system.read.bytes", metrics_data_.system.read_bytes));
      metrics_map_.push_back(new MetricsItem(
            "system.write.bytes", metrics_data_.system.write_bytes));
      metrics_map_.push_back(new MetricsItem(
            "system.signals.received", metrics_data_.system.signals_received));
      metrics_map_.push_back(new MetricsItem(
            "system.signals.processed", metrics_data_.system.signals_processed));
      metrics_map_.push_back(new MetricsItem(
            "system.threads.created", metrics_data_.system.threads_created));
      metrics_map_.push_back(new MetricsItem(
            "system.threads.destroyed", metrics_data_.system.threads_destroyed));

    }

    void Metrics::init_ruby_metrics(STATE) {
      LookupTable* map = LookupTable::create(state);
      Module* mod = as<Module>(G(rubinius)->get_const(state, state->symbol("Metrics")));
      mod->set_const(state, "Map", map);

      Tuple* values = Tuple::create(state, metrics_map_.size());
      values_.set(values);
      mod->set_const(state, "Values", values);

      int index = 0;

      for(MetricsMap::iterator i = metrics_map_.begin();
          i != metrics_map_.end();
          ++i)
      {
        values->put(state, index, Integer::from(state, (*i)->second));

        Object* key = reinterpret_cast<Object*>(state->symbol((*i)->first.c_str()));
        map->store(state, key, Fixnum::from(index++));
      }
    }

    void Metrics::update_ruby_values(STATE) {
      GCDependent guard(state, 0);

      Tuple* values = values_.get();
      int index = 0;

      for(MetricsMap::iterator i = metrics_map_.begin();
          i != metrics_map_.end();
          ++i)
      {
        values->put(state, index++, Integer::from(state, (*i)->second));
      }
    }

    void Metrics::initialize(STATE) {
      InternalThread::initialize(state);

      enabled_ = true;

      timer_ = new timer::Timer;

      metrics_lock_.init();
      metrics_data_ = MetricsData();
      metrics_history_ = MetricsData();
    }

    void Metrics::wakeup(STATE) {
      InternalThread::wakeup(state);

      if(timer_) {
        timer_->clear();
        timer_->cancel();
      }
    }

    void Metrics::after_fork_child(STATE) {
      if(emitter_) emitter_->reinit();

      InternalThread::after_fork_child(state);
    }

    void Metrics::add_historical_metrics(MetricsData& metrics) {
      if(!enabled_) return;

      {
        utilities::thread::Mutex::LockGuard guard(metrics_lock_);

        metrics_history_.add(metrics);
      }
    }

    void Metrics::log_diagnostics(STATE) {
      state->shared().env()->diagnostics()->log();
    }

    void Metrics::run(STATE) {
      timer_->set(interval_);

      while(!thread_exit_) {
        if(timer_->wait_for_tick() < 0) {
          logger::error("metrics: error waiting for timer, exiting thread");
          break;
        }

        if(thread_exit_) break;

        log_diagnostics(state);

        metrics_data_ = MetricsData();
        ThreadList* threads = state->shared().threads();

        for(ThreadList::iterator i = threads->begin();
            i != threads->end();
            ++i) {
          if(VM* vm = (*i)->as_vm()) {
            metrics_data_.add(vm->metrics());
          }
        }

        if(LLVMState* llvm_state = state->shared().llvm_state) {
          metrics_data_.add(llvm_state->vm()->metrics());
        }

        {
          utilities::thread::Mutex::LockGuard guard(metrics_lock_);

          metrics_data_.add(metrics_history_);
        }

        update_ruby_values(state);

        if(emitter_) emitter_->send_metrics();
      }

      timer_->clear();
    }
  }
}
