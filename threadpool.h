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

//Any类型，可以接收任意类型数据
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

    //提取Any类存储的数据、
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
    //基类类型
    class Base
    {
    public:
        virtual ~Base() = default;
    };

    //派生类类型
    template<typename T>
    class Derive : public Base
    {
    public:
        Derive(T data) : data_(data)
        {}
        T data_;
    };
private:
    //基类指针
    std::unique_ptr<Base> base_;
};

//信号量类型
class Semaphore
{
public:
    Semaphore(int limit = 0) 
    : resLimit_(limit)
    {}
    ~Semaphore() = default;

    //获取一个信号量资源
    void wait()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        //等待信号量有资源，没有资源则阻塞当前线程
        cond_.wait(lock, [&]() {return resLimit_ > 0; });
        resLimit_--;
    }

    //增加一个信号量资源
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

//任务返回值类型
class Task;  //Task类型前置声明
class Result
{
public:
    Result(std::shared_ptr<Task> task, bool isValid = true);
    ~Result() = default;

    //获取任务返回值
    Any get();

    //将any_置为任务返回值
    void setVal(Any any);

private:
    Any any_;                     //存储任务返回值
    Semaphore sem_;               //线程通信信号量
    std::shared_ptr<Task> task_;  //指向对应获取返回值的任务对象
    std::atomic_bool isValid_;    //返回值是否有效
};

//任务抽象基类
class Task
{
public:
    Task();
    ~Task() = default;
    void exec();
    void setResult(Result* res);

    /*   可自定义任意任务类型，
    *    从Task继承，重写run方法实现自定义任务处理
    */
    virtual Any run() = 0;
private:
    Result* result_;
};

//线程池工作模式
enum class PoolMode
{
    MODE_FIXED,  //固定数量的xiancheng
    MODE_CACHED, //线程数量可动态增长
};

//线程类型
class Thread
{
public:
    //线程函数对象类型
    using threadFunc = std::function<void(int)>;

    //线程构造
    Thread(threadFunc func);

    //线程析构
    ~Thread();

    //启动线程
    void start();

    //获取线程id
    int getId() const;

private:
    //线程函数对象
    threadFunc func_;

    static int generateId_;

    /*线程id，唯一标识线程，
    *便于cached模式下在线程列表容器中
    *找到对应超时线程
    */
    int threadId_;

};

//线程池类型
class ThreadPool
{
public:
    //线程池构造
    ThreadPool();

    //线程池析构
    ~ThreadPool();

    //设置线程池工作模式
    void setMode(PoolMode mode);

    //设置cached模式下线程数量阈值
    void setThreadMaxThreshHold(int threshhold);

    //设置task任务队列上限阈值
    void setTaskQueMaxThreshHold(int threshhold);

    //向线程池中提交任务
    Result submitTask(std::shared_ptr<Task> sp);

    //开启线程池, 初始线程数量设置为cpu核心数
    void start(int initThreadSize = std::thread::hardware_concurrency());

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator = (const ThreadPool&) = delete;

private:
    //定义线程函数
    void threadFunc(int threadid);

    //检查线程池运行状态
    bool checkRunningState() const;

private:
    std::unordered_map<int, std::unique_ptr<Thread>> threads_; //线程列表
    size_t initThreadSize_;                        //初始线程数量
    int threadSizeThreshHold_;                     //线程数量上限阈值（限制cached模式创建过多线程）
    std::atomic_int currThreadSize_;               //当前线程数量

    std::queue<std::shared_ptr<Task>> taskQue_;  //任务队列
    std::atomic_uint taskSize_;                  //任务数量
    unsigned int taskQueMaxThreshHold_;          //任务队列数量上限阈值

    std::mutex taskQueMtx_;             //保证任务队列线程安全
    std::condition_variable notFull_;   //当前任务队列不满
    std::condition_variable notEmpty_;  //当前任务队列不空
    std::condition_variable exitCond_;  //等待线程资源全部回收

    PoolMode poolMode_;  //当前线程池工作模式

    std::atomic_bool isPoolRunning_;  //当前线程池启动状态
    std::atomic_uint idleThreadSize_;  //当前线程池中空闲线程数量
};

#endif