#include "scheduler.hpp"
#include "workqueue.hpp"

namespace workstealing {

void stopScheduler() {
  running.store(false);
  tasks_required_sem.signal(1);
}

void startScheduler(std::vector<hpx::naming::id_type> workqueues) {
  auto schedulerReady = std::make_shared<hpx::promise<void> >();

  if (hpx::get_os_thread_count() > 1) {
    hpx::threads::executors::default_executor high_priority_executor(hpx::threads::thread_priority_critical);
    hpx::apply(high_priority_executor, &scheduler, workqueues, schedulerReady);
  } else {
    hpx::apply(&scheduler, workqueues, schedulerReady);
  }

  schedulerReady->get_future().get();
}

void scheduler(std::vector<hpx::naming::id_type> workqueues,
               std::shared_ptr<hpx::promise<void> >readyPromise) {
  auto here = hpx::find_here();
  hpx::naming::id_type last_remote = here; // We use *here* as a sentinel value

  auto distributed = hpx::find_all_localities().size() > 1;

  // Figure out which workqueue is local to this scheduler
  for (auto it = workqueues.begin(); it != workqueues.end(); ++it) {
    if (hpx::get_colocation_id(*it).get() == here) {
      local_workqueue = *it;
      workqueues.erase(it);
      break;
    }
  }

  // Workqueue variables are set up, we can start generating tasks
  readyPromise->set_value();

  auto threads = hpx::get_os_thread_count() == 1 ? 1 : hpx::get_os_thread_count() - 1;
  hpx::threads::executors::current_executor scheduler;

  // Pre-init the sem
  tasks_required_sem.signal(threads);

  while (running) {
    tasks_required_sem.wait();

    // Try local queue first then distributed
    hpx::util::function<void(hpx::naming::id_type)> task;
    task = hpx::async<workstealing::workqueue::steal_action>(local_workqueue).get();
    if (distributed && !task) {
      if (last_remote != here) {
        task = hpx::async<workstealing::workqueue::steal_action>(last_remote).get();
      }

      if (!task) {
        auto victim = workqueues.begin();
        std::advance(victim, std::rand() % workqueues.size());

        task = hpx::async<workstealing::workqueue::steal_action>(*victim).get();
        if (task) {
          last_remote = *victim;
        } else {
          last_remote = here;
        }
      }
    }

    if (task) {
      scheduler.add(hpx::util::bind(task, here));
    } else {
      hpx::this_thread::suspend(10);
      tasks_required_sem.signal();
    }
  }
}
}
