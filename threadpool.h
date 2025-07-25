#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <memory>
#include <queue>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <thread>
#include <future>
#include <chrono>

const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60;  //��λ����

//�̳߳ع���ģʽ
enum class PoolMode
{
    MODE_FIXED,  //�̶�������xiancheng
    MODE_CACHED, //�߳������ɶ�̬����
};

//�߳�����
class Thread
{
public:
    //�̺߳�����������
    using threadFunc = std::function<void(int)>;

    //�̹߳���
    Thread(threadFunc func)
        : func_(func)
        , threadId_(generateId_++)
    {}

    //�߳�����
    ~Thread() = default;

    //�����߳�
    void start()
    {
        //�����߳�ִ���̺߳���
        std::thread t([this]() {this->func_(this->threadId_); });
        t.detach();  //���÷����߳�
    }

    //��ȡ�߳�id
    int getId() const
    {
        return threadId_;
    }

private:
    //�̺߳�������
    threadFunc func_;

    static int generateId_;

    /*�߳�id��Ψһ��ʶ�̣߳�
    *����cachedģʽ�����߳��б�������
    *�ҵ���Ӧ��ʱ�߳�
    */
    int threadId_;

};

//��̬��Ա���������ʼ��
int Thread::generateId_ = 0;

//�̳߳�����
class ThreadPool
{
public:
    //�̳߳ع���
    ThreadPool()
        : initThreadSize_(0)
        , taskSize_(0)
        , threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
        , currThreadSize_(0)
        , taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
        , poolMode_(PoolMode::MODE_FIXED)
        , isPoolRunning_(false)
        , idleThreadSize_(0)
    {}

    //�̳߳�����
    ~ThreadPool()
    {
        isPoolRunning_ = false;

        //�ȴ��̳߳��������̷߳���
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        //���ѵȴ�notEmpty_�������߳�
        notEmpty_.notify_all();
        exitCond_.wait(lock, [&]() {return threads_.size() == 0; });
    }

    //�����̳߳ع���ģʽ
    void setMode(PoolMode mode)
    {
        if (checkRunningState())
        {
            return;
        }
        poolMode_ = mode;
    }

    //����cachedģʽ���߳�������ֵ
    void setThreadMaxThreshHold(int threshhold)
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

    //����task�������������ֵ
    void setTaskQueMaxThreshHold(int threshhold)
    {
        if (checkRunningState())
        {
            return;
        }
        taskQueMaxThreshHold_ = threshhold;
    }

    //���̳߳����ύ����
    template<typename Func, typename... Args>
    auto submitTask(Func&& func, Args... args) -> std::future<std::invoke_result_t<Func, Args...>>
    {
        //������񣬷����������
        using RType = std::invoke_result_t<Func, Args...>;  //invoke_result_t�Ƶ��ɵ��ö����ڸ�������ʱ�ķ�������
        auto task = std::make_shared<std::packaged_task<RType()>>(
            //�۵����������
            [f = std::forward<Func>(func), ...args = std::forward<Args>(args)]()
            mutable {return f(args...); }
        );
        std::future<RType> result = task->get_future();

        //��ȡ��
        std::unique_lock<std::mutex> lock(taskQueMtx_);

        //�ȴ���������д��ڿ��е�����notFull_
        if (!notFull_.wait_for(lock, std::chrono::seconds(1),
            [&]() {return taskQue_.size() < taskQueMaxThreshHold_; }))
        {
            //�ȴ�notFull_1s����δ����
            std::cerr << "task queue is full, submit task fail." << std::endl;
            auto task = std::make_shared<std::packaged_task<RType()>>(
                []()->RType {return RType(); });
            (*task)();
            return task->get_future();
        }

        //���ڿ��У�����������������
        taskQue_.emplace([task]() {(*task)(); });
        taskSize_++;

        //���������������в�Ϊ�գ�֪ͨ�ȴ�����notEmpty_���߳�
        notEmpty_.notify_all();

        /*cachedģʽ���ж��Ƿ���Ҫ�����µ��߳�
        * ���С��������񳡾�����Ҫ�������������������߳�������
        * �ж��Ƿ���Ҫ�����µ��߳�
        */
        if (poolMode_ == PoolMode::MODE_CACHED
            && taskSize_ > idleThreadSize_
            && currThreadSize_ < threadSizeThreshHold_)
        {
            std::cout << ">>> create new thread..." << std::endl;

            //�������߳�
            auto ptr = std::make_unique<Thread>([&](int threadid) {this->threadFunc(threadid); });
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            //�������߳�
            threads_[threadId]->start();
            //���µ�ǰ�߳�����
            currThreadSize_++;
            //���¿����߳�����
            idleThreadSize_++;
        }

        //��������Result����
        return result;
    }
    

    //�����̳߳�, ��ʼ�߳���������Ϊcpu������
    void start(int initThreadSize = std::thread::hardware_concurrency())
    {
        //���̳߳�����״̬��Ϊtrue
        isPoolRunning_ = true;

        //��¼��ʼ�̸߳���
        initThreadSize_ = initThreadSize;
        //��¼��ǰ�߳�����
        currThreadSize_ = initThreadSize;

        //�����̶߳���
        for (int i = 0; i < initThreadSize_; i++)
        {
            //����thread�̶߳���ʱ�����̳߳ض���ĳ�Ա����threadFunc��Thread�̶߳����
            auto ptr = std::make_unique<Thread>([this](int threadid) {this->threadFunc(threadid); });
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
        }

        //���������߳�
        for (int i = 0; i < initThreadSize_; i++)
        {
            threads_[i]->start();
            //��¼��ʼ�����߳�����
            idleThreadSize_++;
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator = (const ThreadPool&) = delete;

private:
    //�����̺߳���
    void threadFunc(int threadid)
    {
        //��¼��һ���̺߳���ִ�е�ʱ��
        auto lastTime = std::chrono::high_resolution_clock().now();

        //��ѭ������֤����ȫ������ٻ����߳�
        for (;;)
        {
            Task task;
            {
                // ��ȡ��
                std::unique_lock<std::mutex> lock(taskQueMtx_);

                std::cout << "tid:" << std::this_thread::get_id()
                    << "Trying to get task from taskqueue..." << std::endl;

                /*���տ����߳�
                * cachedģʽ�£����ڴ����˶���̵߳������߳̿���ʱ��
                * �Ѿ�����60s�����������ǰʱ��-��һ���̺߳���ִ�����ʱ��=60s��
                * ��ʱӦ���ճ���initThreadSize_�����Ĳ����߳�
                */
                while (taskQue_.size() == 0)
                {
                    //���̳߳ؽ����������߳���Դ
                    if (!isPoolRunning_)
                    {
                        //���յ�ǰ�߳�
                        threads_.erase(threadid);

                        std::cout << "threadid:" << std::this_thread::get_id()
                            << "exit!" << std::endl;
                        exitCond_.notify_all();
                        //�̺߳����������߳̽���
                        return;
                    }

                    if (poolMode_ == PoolMode::MODE_CACHED)
                    {
                        //����������ʱ���أ�ÿ1s����һ��
                        if (std::cv_status::timeout ==
                            notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                        {
                            auto now = std::chrono::high_resolution_clock().now();
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                            if (dur.count() >= THREAD_MAX_IDLE_TIME
                                && currThreadSize_ > initThreadSize_)
                            {
                                //���յ�ǰ�߳�
                                threads_.erase(threadid);
                                //���µ�ǰ�߳�����
                                currThreadSize_--;
                                //���¿����߳�����
                                idleThreadSize_--;

                                std::cout << "threadid:" << std::this_thread::get_id()
                                    << "exit!" << std::endl;
                                return;
                            }
                        }
                    }
                    else
                    {
                        //�ȴ�������в�Ϊ�յ�����notEmpty_
                        notEmpty_.wait(lock);
                    }
                }

                //���¿����߳�����
                idleThreadSize_--;

                std::cout << "tid:" << std::this_thread::get_id()
                    << "Get task succeed..." << std::endl;

                //���������ȡ��һ������
                task = taskQue_.front();
                taskQue_.pop();
                taskSize_--;

                //����������Բ�Ϊ�գ�֪ͨ�ȴ�notEmpty_�������߳�
                if (taskQue_.size() > 0)
                {
                    notEmpty_.notify_all();
                }

                //֪ͨ�ȴ�notFull_���߳�
                notFull_.notify_all();
            }

            //��ǰ�̸߳���ִ���������
            if (task != nullptr)
            {
                task();
            }

            //���¿����߳�����
            idleThreadSize_++;
            //�����̺߳���ִ����ϵ�ʱ��
            lastTime = std::chrono::high_resolution_clock().now();
        }
    }

    //����̳߳�����״̬
    bool checkRunningState() const
    {
        return isPoolRunning_;
    }

private:
    std::unordered_map<int, std::unique_ptr<Thread>> threads_; //�߳��б�

    size_t initThreadSize_;                        //��ʼ�߳�����
    int threadSizeThreshHold_;                     //�߳�����������ֵ������cachedģʽ���������̣߳�
    std::atomic_int currThreadSize_;               //��ǰ�߳�����

    using Task = std::function<void()>;
    std::queue<Task> taskQue_;                   //�������
    std::atomic_uint taskSize_;                  //��������
    unsigned int taskQueMaxThreshHold_;          //�����������������ֵ

    std::mutex taskQueMtx_;             //��֤��������̰߳�ȫ
    std::condition_variable notFull_;   //��ǰ������в���
    std::condition_variable notEmpty_;  //��ǰ������в���
    std::condition_variable exitCond_;  //�ȴ��߳���Դȫ������

    PoolMode poolMode_;  //��ǰ�̳߳ع���ģʽ

    std::atomic_bool isPoolRunning_;   //��ǰ�̳߳�����״̬
    std::atomic_uint idleThreadSize_;  //��ǰ�̳߳��п����߳�����
};

#endif