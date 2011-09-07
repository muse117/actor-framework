#include <atomic>
#include <iostream>

#include "cppa/on.hpp"
#include "cppa/context.hpp"
#include "cppa/scheduler.hpp"
#include "cppa/to_string.hpp"

#include "cppa/detail/thread.hpp"
#include "cppa/detail/actor_count.hpp"
#include "cppa/detail/mock_scheduler.hpp"
#include "cppa/detail/singleton_manager.hpp"
#include "cppa/detail/thread_pool_scheduler.hpp"
#include "cppa/detail/converted_thread_context.hpp"

namespace {

typedef std::uint32_t ui32;

struct exit_observer : cppa::attachable
{
    ~exit_observer()
    {
        cppa::detail::dec_actor_count();
    }
};

} // namespace <anonymous>

namespace cppa {

struct scheduler_helper
{

    typedef intrusive_ptr<detail::converted_thread_context> ptr_type;

    scheduler_helper() : m_worker(new detail::converted_thread_context)
    {
    }

    void start()
    {
        m_thread = detail::thread(&scheduler_helper::time_emitter, m_worker);
    }

    void stop()
    {
        {
            any_tuple content = make_tuple(atom(":_DIE"));
            m_worker->enqueue(message(m_worker, m_worker, content));
        }
        m_thread.join();
    }

    ptr_type m_worker;
    detail::thread m_thread;

 private:

    static void time_emitter(ptr_type m_self);

};

void scheduler_helper::time_emitter(scheduler_helper::ptr_type m_self)
{
    // setup & local variables
    set_self(m_self.get());
    auto& queue = m_self->m_mailbox.queue();
    typedef std::pair<cppa::actor_ptr, cppa::any_tuple> future_msg;
    std::multimap<decltype(detail::now()), future_msg> messages;
    decltype(queue.pop()) msg_ptr = nullptr;
    decltype(detail::now()) now;
    bool done = false;
    // message handling rules
    auto rules =
    (
        on<util::duration, any_type*>() >> [&](util::duration d)
        {
            any_tuple tup = msg_ptr->msg.content().tail(1);
            if (!tup.empty())
            {
                // calculate timeout
                auto timeout = detail::now();
                timeout += d;
                future_msg fmsg(msg_ptr->msg.sender(), tup);
                messages.insert(std::make_pair(std::move(timeout),
                                               std::move(fmsg)));
            }
        },
        on<atom(":_DIE")>() >> [&]()
        {
            done = true;
        }
    );
    // loop
    while (!done)
    {
        while (msg_ptr == nullptr)
        {
            if (messages.empty())
            {
                msg_ptr = queue.pop();
            }
            else
            {
                now = detail::now();
                // handle timeouts (send messages)
                auto it = messages.begin();
                while (it != messages.end() && (it->first) <= now)
                {
                    auto& whom = (it->second).first;
                    auto& what = (it->second).second;
                    whom->enqueue(message(whom, whom, what));
                    messages.erase(it);
                    it = messages.begin();
                }
                // wait for next message or next timeout
                if (it != messages.end())
                {
                    msg_ptr = queue.try_pop(it->first);
                }
            }
        }
        rules(msg_ptr->msg.content());
        delete msg_ptr;
        msg_ptr = nullptr;
    }
}

scheduler::scheduler() : m_helper(new scheduler_helper)
{
}

void scheduler::start()
{
    m_helper->start();
}

void scheduler::stop()
{
    m_helper->stop();
}

scheduler::~scheduler()
{
    delete m_helper;
}

channel* scheduler::future_send_helper()
{
    return m_helper->m_worker.get();
}

void scheduler::await_others_done()
{
    detail::actor_count_wait_until((unchecked_self() == nullptr) ? 0 : 1);
}

void scheduler::register_converted_context(context* what)
{
    if (what)
    {
        detail::inc_actor_count();
        what->attach(new exit_observer);
    }
}

attachable* scheduler::register_hidden_context()
{
    detail::inc_actor_count();
    return new exit_observer;
}

void scheduler::exit_context(context* ctx, std::uint32_t reason)
{
    ctx->quit(reason);
}

void set_scheduler(scheduler* sched)
{
    if (detail::singleton_manager::set_scheduler(sched) == false)
    {
        throw std::runtime_error("scheduler already set");
    }
}

scheduler* get_scheduler()
{
    scheduler* result = detail::singleton_manager::get_scheduler();
    if (result == nullptr)
    {
        result = new detail::thread_pool_scheduler;
        try
        {
            set_scheduler(result);
        }
        catch (std::runtime_error&)
        {
            delete result;
            return detail::singleton_manager::get_scheduler();
        }
    }
    return result;
}

} // namespace cppa
