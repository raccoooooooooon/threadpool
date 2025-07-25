#include "threadpool.h"

#include <thread>
#include <iostream>
#include <chrono>

const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60;  //单位：秒

//////////////////////// 任务方法实现 ////////////////////////

Task::Task()
    : result_(nullptr)
{
}

void Task::exec()
{
    if (result_ != nullptr)
    {
        result_->setVal(run());
    }
}

void Task::setResult(Result* res)
{
    result_ = res;
}

//////////////////////// 线程池方法实现 ////////////////////////

//线程池构造
ThreadPool::ThreadPool()
    : initThreadSize_(0)
    , taskSize_(0)
    , threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
    , currThreadSize_(0)
    , taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
    , poolMode_(PoolMode::MODE_FIXED)
    , isPoolRunning_(false)
    , idleThreadSize_(0)
{
}

//线程池析构
ThreadPool::~ThreadPool()
{
    isPoolRunning_ = false;

    //等待线程池中所有线程返回
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    //唤醒等待notEmpty_的所有线程
    notEmpty_.notify_all();
    exitCond_.wait(lock, [&]() {return threads_.size() == 0; });
}

//设置线程池工作模式
void ThreadPool::setMode(PoolMode mode)
{
    if (checkRunningState())
    {
        return;
    }
    poolMode_ = mode;
}

//设置cached模式下线程数量阈值
void ThreadPool::setThreadMaxThreshHold(int threshhold)
{
    if (checkRunningState())
    {
        return;
    }
    if (poolMode_ == PoolMode::MODE_CACHED)
    {
        threadSizeThreshHold_ = threshhold;
    }
}

//设置task任务队列上限阈值
void ThreadPool::setTaskQueMaxThreshHold(int threshhold)
{
    if (checkRunningState())
    {
        return;
    }
    taskQueMaxThreshHold_ = threshhold;
}

//向线程池中提交任务
Result ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
    //获取锁
    std::unique_lock<std::mutex> lock(taskQueMtx_);

    //等待任务队列中存在空闲的条件notFull_
    if (!notFull_.wait_for(lock, std::chrono::seconds(1),
        [&]() {return taskQue_.size() < taskQueMaxThreshHold_; }))
    {
        //等待notFull_1s，仍未满足
        std::cerr << "task queue is full, submit task fail." << std::endl;
        return Result(sp, false);
    }

    //存在空闲，将任务加入任务队列
    taskQue_.emplace(sp);
    taskSize_++;

    //加入任务后，任务队列不为空，通知等待所有notEmpty_的线程
    notEmpty_.notify_all();

    /*cached模式，判断是否需要创建新的线程
    * 针对小而快的任务场景，需要根据任务数量及空闲线程数量，
    * 判断是否需要创建新的线程
    */
    if (poolMode_ == PoolMode::MODE_CACHED
        && taskSize_ > idleThreadSize_
        && currThreadSize_ < threadSizeThreshHold_)
    {
        std::cout << ">>> create new thread..." << std::endl;

        //创建新线程
        auto ptr = std::make_unique<Thread>([&](int threadid) {this->threadFunc(threadid); });
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
        //启动新线程
        threads_[threadId]->start();
        //更新当前线程数量
        currThreadSize_++;
        //更新空闲线程数量
        idleThreadSize_++;
    }

    //返回任务Result对象
    return Result(sp);
}

//开启线程池
void ThreadPool::start(int initThreadSize)
{
    //将线程池启动状态置为true
    isPoolRunning_ = true;

    //记录初始线程个数
    initThreadSize_ = initThreadSize;
    //记录当前线程数量
    currThreadSize_ = initThreadSize;

    //创建线程对象
    for (int i = 0; i < initThreadSize_; i++)
    {
        //创建thread线程对象时，将线程池对象的成员函数threadFunc与Thread线程对象绑定
        auto ptr = std::make_unique<Thread>([this](int threadid) {this->threadFunc(threadid); });
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
    }

    //启动所有线程
    for (int i = 0; i < initThreadSize_; i++)
    {
        threads_[i]->start();
        //记录初始空闲线程数量
        idleThreadSize_++;
    }
}

//定义线程函数，线程池中所有线程作为消费者消费任务队列中的任务
void ThreadPool::threadFunc(int threadid)
{
    //记录上一次线程函数执行的时间
    auto lastTime = std::chrono::high_resolution_clock().now();

    //死循环，保证任务全部完成再回收线程
    for(;;)
    {
        std::shared_ptr<Task> task;
        {
            // 获取锁
            std::unique_lock<std::mutex> lock(taskQueMtx_);

            std::cout << "tid:" << std::this_thread::get_id()
                << "Trying to get task from taskqueue..." << std::endl;

            /*回收空闲线程
            * cached模式下，存在创建了多个线程但部分线程空闲时间
            * 已经超过60s的情况，（当前时间-上一次线程函数执行完毕时间=60s）
            * 此时应回收超过initThreadSize_数量的部分线程
            */
            while (taskQue_.size() == 0)
            {
                //若线程池结束，回收线程资源
                if (!isPoolRunning_)
                {
                    //回收当前线程
                    threads_.erase(threadid);

                    std::cout << "threadid:" << std::this_thread::get_id()
                        << "exit!" << std::endl;
                    exitCond_.notify_all();
                    //线程函数结束，线程结束
                    return;
                }

                if (poolMode_ == PoolMode::MODE_CACHED)
                {
                    //条件变量超时返回，每1s返回一次
                    if (std::cv_status::timeout ==
                        notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                    {
                        auto now = std::chrono::high_resolution_clock().now();
                        auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                        if (dur.count() >= THREAD_MAX_IDLE_TIME
                            && currThreadSize_ > initThreadSize_)
                        {
                            //回收当前线程
                            threads_.erase(threadid);
                            //更新当前线程数量
                            currThreadSize_--;
                            //更新空闲线程数量
                            idleThreadSize_--;

                            std::cout << "threadid:" << std::this_thread::get_id()
                                << "exit!" << std::endl;
                            return;
                        }
                    }
                }
                else
                {
                    //等待任务队列不为空的条件notEmpty_
                    notEmpty_.wait(lock);
                }

                //线程池结束，回收线程资源
                //if (!isPoolRunning_)
                //{
                //    //回收当前线程
                //    threads_.erase(threadid);

                //    std::cout << "threadid:" << std::this_thread::get_id()
                //        << "exit!" << std::endl;
                //    exitCond_.notify_all();
                //    return;
                //}
            }

            //更新空闲线程数量
            idleThreadSize_--;

            std::cout << "tid:" << std::this_thread::get_id()
                << "Get task succeed..." << std::endl;

            //从任务队列取出一个任务
            task = taskQue_.front();
            taskQue_.pop();
            taskSize_--;

            //若任务队列仍不为空，通知等待notEmpty_的所有线程
            if (taskQue_.size() > 0)
            {
                notEmpty_.notify_all();
            }

            //通知等待notFull_的线程
            notFull_.notify_all();
        }

        //当前线程负责执行这个任务
        if (task != nullptr)
        {
            //task->run();
            task->exec();
        }

        //更新空闲线程数量
        idleThreadSize_++;
        //更新线程函数执行完毕的时间
        lastTime = std::chrono::high_resolution_clock().now();
    }
}

//检查线程池运行状态
bool ThreadPool::checkRunningState() const
{
    return isPoolRunning_;
}

//////////////////////// 线程方法实现 ////////////////////////

int Thread::generateId_ = 0;

//线程构造
Thread::Thread(threadFunc func)
    : func_(func)
    , threadId_(generateId_++)
{
}

//线程析构
Thread::~Thread()
{
}

//启动线程
void Thread::start()
{
    //创建线程执行线程函数
    std::thread t([this]() {this->func_(this->threadId_); });
    t.detach();  //设置分离线程
}

//获取线程id
int Thread::getId() const
{
    return threadId_;
}

//////////////////////// 返回值方法实现 ////////////////////////

Result::Result(std::shared_ptr<Task> task, bool isValid)
    : isValid_(isValid)
    , task_(task)
{
    task_->setResult(this);
}

//获取任务返回值
Any Result::get()
{
    if (!isValid_)
    {
        return "";
    }

    //阻塞等待任务执行完毕信号
    sem_.wait();
    return std::move(any_);
}

//将any_置为任务返回值
void Result::setVal(Any any)
{
    //存储task返回值
    this->any_ = std::move(any);
    //获取任务返回值后增加信号量资源
    sem_.post();

}