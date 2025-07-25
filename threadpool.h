#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <memory>
#include <queue>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

/*
* example:
* ThreadPool pool;
* pool.start(4);
* 
* class MyTask : public Task
*{
*public:
*    void run(){//task code...} 
*};
* pool.submitTask(std::make_shared<MyTask>());
*/

//Any���ͣ����Խ���������������
class Any
{
public:
    Any() = default;
    ~Any() = default;
    Any(const Any&) = delete;
    Any& operator=(const Any&) = delete;
    Any(Any&&) = default;
    Any& operator=(Any&&) = default;

    template<typename T>
    Any(T data) : base_(std::make_unique<Derive<T>>(data))
    {}

    //��ȡAny��洢�����ݡ�
    template<typename T>
    T cast_()
    {
        Derive<T>* pd = dynamic_cast<Derive<T>*>(base_.get());
        if (pd == nullptr)
        {
            throw "type is unmatch!";
        }
        return pd->data_;
    }
private:
    //��������
    class Base
    {
    public:
        virtual ~Base() = default;
    };

    //����������
    template<typename T>
    class Derive : public Base
    {
    public:
        Derive(T data) : data_(data)
        {}
        T data_;
    };
private:
    //����ָ��
    std::unique_ptr<Base> base_;
};

//�ź�������
class Semaphore
{
public:
    Semaphore(int limit = 0) 
    : resLimit_(limit)
    {}
    ~Semaphore() = default;

    //��ȡһ���ź�����Դ
    void wait()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        //�ȴ��ź�������Դ��û����Դ��������ǰ�߳�
        cond_.wait(lock, [&]() {return resLimit_ > 0; });
        resLimit_--;
    }

    //����һ���ź�����Դ
    void post()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        resLimit_++;
        cond_.notify_all();
    }

private:
    int resLimit_;
    std::mutex mtx_;
    std::condition_variable cond_;
};

//���񷵻�ֵ����
class Task;  //Task����ǰ������
class Result
{
public:
    Result(std::shared_ptr<Task> task, bool isValid = true);
    ~Result() = default;

    //��ȡ���񷵻�ֵ
    Any get();

    //��any_��Ϊ���񷵻�ֵ
    void setVal(Any any);

private:
    Any any_;                     //�洢���񷵻�ֵ
    Semaphore sem_;               //�߳�ͨ���ź���
    std::shared_ptr<Task> task_;  //ָ���Ӧ��ȡ����ֵ���������
    std::atomic_bool isValid_;    //����ֵ�Ƿ���Ч
};

//����������
class Task
{
public:
    Task();
    ~Task() = default;
    void exec();
    void setResult(Result* res);

    /*   ���Զ��������������ͣ�
    *    ��Task�̳У���дrun����ʵ���Զ���������
    */
    virtual Any run() = 0;
private:
    Result* result_;
};

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
    Thread(threadFunc func);

    //�߳�����
    ~Thread();

    //�����߳�
    void start();

    //��ȡ�߳�id
    int getId() const;

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

//�̳߳�����
class ThreadPool
{
public:
    //�̳߳ع���
    ThreadPool();

    //�̳߳�����
    ~ThreadPool();

    //�����̳߳ع���ģʽ
    void setMode(PoolMode mode);

    //����cachedģʽ���߳�������ֵ
    void setThreadMaxThreshHold(int threshhold);

    //����task�������������ֵ
    void setTaskQueMaxThreshHold(int threshhold);

    //���̳߳����ύ����
    Result submitTask(std::shared_ptr<Task> sp);

    //�����̳߳�, ��ʼ�߳���������Ϊcpu������
    void start(int initThreadSize = std::thread::hardware_concurrency());

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator = (const ThreadPool&) = delete;

private:
    //�����̺߳���
    void threadFunc(int threadid);

    //����̳߳�����״̬
    bool checkRunningState() const;

private:
    std::unordered_map<int, std::unique_ptr<Thread>> threads_; //�߳��б�
    size_t initThreadSize_;                        //��ʼ�߳�����
    int threadSizeThreshHold_;                     //�߳�����������ֵ������cachedģʽ���������̣߳�
    std::atomic_int currThreadSize_;               //��ǰ�߳�����

    std::queue<std::shared_ptr<Task>> taskQue_;  //�������
    std::atomic_uint taskSize_;                  //��������
    unsigned int taskQueMaxThreshHold_;          //�����������������ֵ

    std::mutex taskQueMtx_;             //��֤��������̰߳�ȫ
    std::condition_variable notFull_;   //��ǰ������в���
    std::condition_variable notEmpty_;  //��ǰ������в���
    std::condition_variable exitCond_;  //�ȴ��߳���Դȫ������

    PoolMode poolMode_;  //��ǰ�̳߳ع���ģʽ

    std::atomic_bool isPoolRunning_;  //��ǰ�̳߳�����״̬
    std::atomic_uint idleThreadSize_;  //��ǰ�̳߳��п����߳�����
};

#endif