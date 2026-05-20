#include "EventLoop.hpp"
#include "EventLoopThread.hpp"
#include "EventLoopThreadPool.hpp"
#include <assert.h> 
#include <memory>

namespace net{
EventLoopThreadPool::EventLoopThreadPool(EventLoop *baseLoop)
    : baseLoop_(baseLoop)
    , start_(false)
    , numThreads_(0)
    , next_(0)
    {
    }

EventLoopThreadPool::~EventLoopThreadPool(){}

void EventLoopThreadPool::start(const ThreadInitCallback& cb){
    assert(!start_);
    baseLoop_->assertInLoopThread();

    start_ = true;

    for (int i = 0; i < numThreads_; i++){
        auto t = std::make_unique<EventLoopThread>();
        loops_.push_back(t->startLoop());
        threads_.push_back(std::move(t));
    }
    if (numThreads_ == 0 && cb){
        cb(baseLoop_);
    } 
}

EventLoop* EventLoopThreadPool::getNextLoop(){
    assert(start_);
    baseLoop_->assertInLoopThread();
    EventLoop *loop = baseLoop_;

    if (!loops_.empty()){
        loop = loops_[next_++];
        if (next_ > loops_.size()){
            next_ = 0;
        }
    }
    return loop;
}

}