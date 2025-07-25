#include "threadpool.h"

#include <thread>
#include <iostream>
#include <chrono>

const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60;  //��λ����

//////////////////////// ���񷽷�ʵ�� ////////////////////////

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

//////////////////////// �̳߳ط���ʵ�� ////////////////////////

//�̳߳ع���
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

//�̳߳�����
ThreadPool::~ThreadPool()
{
    isPoolRunning_ = false;

    //�ȴ��̳߳��������̷߳���
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    //���ѵȴ�notEmpty_�������߳�
    notEmpty_.notify_all();
    exitCond_.wait(lock, [&]() {return threads_.size() == 0; });
}

//�����̳߳ع���ģʽ
void ThreadPool::setMode(PoolMode mode)
{
    if (checkRunningState())
    {
        return;
    }
    poolMode_ = mode;
}

//����cachedģʽ���߳�������ֵ
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

//����task�������������ֵ
void ThreadPool::setTaskQueMaxThreshHold(int threshhold)
{
    if (checkRunningState())
    {
        return;
    }
    taskQueMaxThreshHold_ = threshhold;
}

//���̳߳����ύ����
Result ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
    //��ȡ��
    std::unique_lock<std::mutex> lock(taskQueMtx_);

    //�ȴ���������д��ڿ��е�����notFull_
    if (!notFull_.wait_for(lock, std::chrono::seconds(1),
        [&]() {return taskQue_.size() < taskQueMaxThreshHold_; }))
    {
        //�ȴ�notFull_1s����δ����
        std::cerr << "task queue is full, submit task fail." << std::endl;
        return Result(sp, false);
    }

    //���ڿ��У�����������������
    taskQue_.emplace(sp);
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
    return Result(sp);
}

//�����̳߳�
void ThreadPool::start(int initThreadSize)
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

//�����̺߳������̳߳��������߳���Ϊ������������������е�����
void ThreadPool::threadFunc(int threadid)
{
    //��¼��һ���̺߳���ִ�е�ʱ��
    auto lastTime = std::chrono::high_resolution_clock().now();

    //��ѭ������֤����ȫ������ٻ����߳�
    for(;;)
    {
        std::shared_ptr<Task> task;
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

                //�̳߳ؽ����������߳���Դ
                //if (!isPoolRunning_)
                //{
                //    //���յ�ǰ�߳�
                //    threads_.erase(threadid);

                //    std::cout << "threadid:" << std::this_thread::get_id()
                //        << "exit!" << std::endl;
                //    exitCond_.notify_all();
                //    return;
                //}
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
            //task->run();
            task->exec();
        }

        //���¿����߳�����
        idleThreadSize_++;
        //�����̺߳���ִ����ϵ�ʱ��
        lastTime = std::chrono::high_resolution_clock().now();
    }
}

//����̳߳�����״̬
bool ThreadPool::checkRunningState() const
{
    return isPoolRunning_;
}

//////////////////////// �̷߳���ʵ�� ////////////////////////

int Thread::generateId_ = 0;

//�̹߳���
Thread::Thread(threadFunc func)
    : func_(func)
    , threadId_(generateId_++)
{
}

//�߳�����
Thread::~Thread()
{
}

//�����߳�
void Thread::start()
{
    //�����߳�ִ���̺߳���
    std::thread t([this]() {this->func_(this->threadId_); });
    t.detach();  //���÷����߳�
}

//��ȡ�߳�id
int Thread::getId() const
{
    return threadId_;
}

//////////////////////// ����ֵ����ʵ�� ////////////////////////

Result::Result(std::shared_ptr<Task> task, bool isValid)
    : isValid_(isValid)
    , task_(task)
{
    task_->setResult(this);
}

//��ȡ���񷵻�ֵ
Any Result::get()
{
    if (!isValid_)
    {
        return "";
    }

    //�����ȴ�����ִ������ź�
    sem_.wait();
    return std::move(any_);
}

//��any_��Ϊ���񷵻�ֵ
void Result::setVal(Any any)
{
    //�洢task����ֵ
    this->any_ = std::move(any);
    //��ȡ���񷵻�ֵ�������ź�����Դ
    sem_.post();

}