#ifndef BOOST_THREAD_PTHREAD_THREAD_DATA_HPP
#define BOOST_THREAD_PTHREAD_THREAD_DATA_HPP
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.lslboost.org/LICENSE_1_0.txt)
// (C) Copyright 2007 Anthony Williams
// (C) Copyright 2011-2012 Vicente J. Botet Escriba

#include <lslboost/thread/detail/config.hpp>
#include <lslboost/thread/exceptions.hpp>
#include <lslboost/thread/lock_guard.hpp>
#include <lslboost/thread/lock_types.hpp>
#include <lslboost/thread/mutex.hpp>
#include <lslboost/thread/pthread/condition_variable_fwd.hpp>

#include <lslboost/shared_ptr.hpp>
#include <lslboost/enable_shared_from_this.hpp>
#include <lslboost/optional.hpp>
#include <lslboost/assert.hpp>
#ifdef BOOST_THREAD_USES_CHRONO
#include <lslboost/chrono/system_clocks.hpp>
#endif

#include <map>
#include <vector>
#include <utility>

#if defined(__ANDROID__)
#include <asm/page.h> // http://code.google.com/p/android/issues/detail?id=39983
#endif

#include <pthread.h>
#include <unistd.h>

#include <lslboost/config/abi_prefix.hpp>

namespace lslboost
{
    class thread_attributes {
    public:
        thread_attributes() BOOST_NOEXCEPT {
            int res = pthread_attr_init(&val_);
            BOOST_VERIFY(!res && "pthread_attr_init failed");
        }
        ~thread_attributes() {
          int res = pthread_attr_destroy(&val_);
          BOOST_VERIFY(!res && "pthread_attr_destroy failed");
        }
        // stack
        void set_stack_size(std::size_t size) BOOST_NOEXCEPT {
          if (size==0) return;
          std::size_t page_size = getpagesize();
#ifdef PTHREAD_STACK_MIN
          if (size<PTHREAD_STACK_MIN) size=PTHREAD_STACK_MIN;
#endif
          size = ((size+page_size-1)/page_size)*page_size;
          int res = pthread_attr_setstacksize(&val_, size);
          BOOST_VERIFY(!res && "pthread_attr_setstacksize failed");
        }

        std::size_t get_stack_size() const BOOST_NOEXCEPT {
            std::size_t size;
            int res = pthread_attr_getstacksize(&val_, &size);
            BOOST_VERIFY(!res && "pthread_attr_getstacksize failed");
            return size;
        }
#define BOOST_THREAD_DEFINES_THREAD_ATTRIBUTES_NATIVE_HANDLE

        typedef pthread_attr_t native_handle_type;
        native_handle_type* native_handle() BOOST_NOEXCEPT {
          return &val_;
        }
        const native_handle_type* native_handle() const BOOST_NOEXCEPT {
          return &val_;
        }

    private:
        pthread_attr_t val_;
    };

    class thread;

    namespace detail
    {
        struct shared_state_base;
        struct tss_cleanup_function;
        struct thread_exit_callback_node;
        struct tss_data_node
        {
            lslboost::shared_ptr<lslboost::detail::tss_cleanup_function> func;
            void* value;

            tss_data_node(lslboost::shared_ptr<lslboost::detail::tss_cleanup_function> func_,
                          void* value_):
                func(func_),value(value_)
            {}
        };

        struct thread_data_base;
        typedef lslboost::shared_ptr<thread_data_base> thread_data_ptr;

        struct BOOST_THREAD_DECL thread_data_base:
            enable_shared_from_this<thread_data_base>
        {
            thread_data_ptr self;
            pthread_t thread_handle;
            lslboost::mutex data_mutex;
            lslboost::condition_variable done_condition;
            lslboost::mutex sleep_mutex;
            lslboost::condition_variable sleep_condition;
            bool done;
            bool join_started;
            bool joined;
            lslboost::detail::thread_exit_callback_node* thread_exit_callbacks;
            std::map<void const*,lslboost::detail::tss_data_node> tss_data;

            pthread_mutex_t* cond_mutex;
            pthread_cond_t* current_cond;
            typedef std::vector<std::pair<condition_variable*, mutex*>
            //, hidden_allocator<std::pair<condition_variable*, mutex*> >
            > notify_list_t;
            notify_list_t notify;

            typedef std::vector<shared_ptr<shared_state_base> > async_states_t;
            async_states_t async_states_;

//#if defined BOOST_THREAD_PROVIDES_INTERRUPTIONS
            // These data must be at the end so that the access to the other fields doesn't change
            // when BOOST_THREAD_PROVIDES_INTERRUPTIONS is defined.
            // Another option is to have them always
            bool interrupt_enabled;
            bool interrupt_requested;
//#endif
            thread_data_base():
                thread_handle(0),
                done(false),join_started(false),joined(false),
                thread_exit_callbacks(0),
                cond_mutex(0),
                current_cond(0),
                notify(),
                async_states_()
//#if defined BOOST_THREAD_PROVIDES_INTERRUPTIONS
                , interrupt_enabled(true)
                , interrupt_requested(false)
//#endif
            {}
            virtual ~thread_data_base();

            typedef pthread_t native_handle_type;

            virtual void run()=0;
            virtual void notify_all_at_thread_exit(condition_variable* cv, mutex* m)
            {
              notify.push_back(std::pair<condition_variable*, mutex*>(cv, m));
            }

            void make_ready_at_thread_exit(shared_ptr<shared_state_base> as)
            {
              async_states_.push_back(as);
            }

        };

        BOOST_THREAD_DECL thread_data_base* get_current_thread_data();

#if defined BOOST_THREAD_PROVIDES_INTERRUPTIONS
        class interruption_checker
        {
            thread_data_base* const thread_info;
            pthread_mutex_t* m;
            bool set;

            void check_for_interruption()
            {
#ifndef BOOST_NO_EXCEPTIONS
                if(thread_info->interrupt_requested)
                {
                    thread_info->interrupt_requested=false;
                    throw thread_interrupted(); // BOOST_NO_EXCEPTIONS protected
                }
#endif
            }

            void operator=(interruption_checker&);
        public:
            explicit interruption_checker(pthread_mutex_t* cond_mutex,pthread_cond_t* cond):
                thread_info(detail::get_current_thread_data()),m(cond_mutex),
                set(thread_info && thread_info->interrupt_enabled)
            {
                if(set)
                {
                    lock_guard<mutex> guard(thread_info->data_mutex);
                    check_for_interruption();
                    thread_info->cond_mutex=cond_mutex;
                    thread_info->current_cond=cond;
                    BOOST_VERIFY(!pthread_mutex_lock(m));
                }
                else
                {
                    BOOST_VERIFY(!pthread_mutex_lock(m));
                }
            }
            ~interruption_checker()
            {
                if(set)
                {
                    BOOST_VERIFY(!pthread_mutex_unlock(m));
                    lock_guard<mutex> guard(thread_info->data_mutex);
                    thread_info->cond_mutex=NULL;
                    thread_info->current_cond=NULL;
                }
                else
                {
                    BOOST_VERIFY(!pthread_mutex_unlock(m));
                }
            }
        };
#endif
    }

    namespace this_thread
    {
      namespace hiden
      {
        void BOOST_THREAD_DECL sleep_for(const timespec& ts);
        void BOOST_THREAD_DECL sleep_until(const timespec& ts);
      }

#ifdef BOOST_THREAD_USES_CHRONO
#ifdef BOOST_THREAD_SLEEP_FOR_IS_STEADY

        inline
        void BOOST_SYMBOL_VISIBLE sleep_for(const chrono::nanoseconds& ns)
        {
            return lslboost::this_thread::hiden::sleep_for(lslboost::detail::to_timespec(ns));
        }
#endif
#endif // BOOST_THREAD_USES_CHRONO

        void BOOST_THREAD_DECL yield() BOOST_NOEXCEPT;

#if defined BOOST_THREAD_USES_DATETIME
#ifdef __DECXXX
        /// Workaround of DECCXX issue of incorrect template substitution
        template<>
#endif
        inline void sleep(system_time const& abs_time)
        {
          return lslboost::this_thread::hiden::sleep_until(lslboost::detail::to_timespec(abs_time));
        }

        template<typename TimeDuration>
        inline BOOST_SYMBOL_VISIBLE void sleep(TimeDuration const& rel_time)
        {
            this_thread::sleep(get_system_time()+rel_time);
        }
#endif // BOOST_THREAD_USES_DATETIME
    } // this_thread
}

#include <lslboost/config/abi_suffix.hpp>

#endif
