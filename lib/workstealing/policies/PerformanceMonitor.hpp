#ifndef Performance_Moitor_HPP
#define Performance_Moitor_HPP

#include "workstealing/channels/SchedulerChannels.hpp"

#include <hpx/parallel/algorithms/sort.hpp>
#include <hpx/include/components.hpp>
#include <hpx/parallel/task_group.hpp>
#include <hpx/execution.hpp>
#include <hpx/iostream.hpp>
#include <atomic>

namespace Workstealing
{
    namespace Scheduler {
        //extern std::shared_ptr<SchedulerChannelHolder> schedulerChannelHolder;
    }
	namespace Policies
	{
		//namespace PerformancePolicyPerf
		//{
		//	void registerPerformanceCounters();
		//}


		class PerformanceMonitor{

		private:
            //channels
            //hpx::components::client<SchedulerChannelHolder> schedulerChannelHolder;

			//node id
			hpx::id_type& local_workpool;
			std::vector<hpx::id_type>& distributed_workpools;

			//node info
            struct NodeInfo {
                hpx::id_type id;
                double cpuLoad;
                double latency;
                int remainingTasks;
                // Add other information here
            };

            struct CompareNodeInfo {
                bool operator()(const NodeInfo& a,
                                const NodeInfo& b) const {
                // Compare a and b here
                // For example, sort by CPU load
                return a.cpuLoad < b.cpuLoad;
                }
            };
            
            std::vector<NodeInfo> nodeInfoVector;

			//compute
            void compareNode();

            //init
		    void generateNodeInfoVector();
            void generateChannels();


            //---get info---
            hpx::execution::experimental::task_group infoTasks;

            hpx::execution::parallel_executor top_priority_executor = hpx::execution::parallel_executor(hpx::threads::thread_priority::bound);

            void task_group_run_with_executor(hpx::execution::experimental::task_group& tg, hpx::execution::parallel_executor& executor, std::function<void()> func) {
                tg.run([&](){
                    return hpx::async(executor, func).get();
                });
            }

            //refresh data
            bool refreshInfo();
            bool autoRefreshInfo();

            void refreshCpuLoad();
            void refreshSchedularInfo();


		public:
			PerformanceMonitor(hpx::id_type& local_workpool, std::vector<hpx::id_type>& distributed_workpools);

            std::atomic<bool> performanceMonitorRunning;

            // init

            void start() {
                hpx::async(top_priority_executor, [&]() { autoRefreshInfo(); });
            }

            void init();

            void stop() {
                performanceMonitorRunning.store(false);
                //hpx::cout << "stopSchedulers2:" << hpx::get_locality_name()<< performanceMonitorRunning << std::endl;
            }

            //get ids
            hpx::id_type getTopWorthStealId();
            
            //channel
        };

	}
}


#endif
