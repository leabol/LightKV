#include "EventLoop.hpp"

#include <sys/eventfd.h>

#include <cassert>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "Channel.hpp"
#include "EpollPoller.hpp"
#include "Log.hpp"

using namespace net;

namespace {
const int kPollTimeMs = 10000;
}

EventLoop::EventLoop() :
    looping_(false), quit_(false), eventHandling_(false), callingPendingFunctors_(false),
    threadId_(std::this_thread::get_id()), poller_(std::make_unique<EpollPoller>()),
    wakeupFd_(createEventFd()), wakeupChannel_(std::make_unique<Channel>(this, wakeupFd_)) {
  wakeupChannel_->setReadCallback([this]() { this->handelWakeup(); });
  wakeupChannel_->enableReading();
  LOG_TRACE("EventLoop is created");
}

EventLoop::~EventLoop() {
  wakeupChannel_->disableAll();
  wakeupChannel_->remove();
  ::close(wakeupFd_);
}

void EventLoop::loop() {
  assert(!looping_);
  assertInLoopThread();
  looping_ = true;
  quit_ = false;
  LOG_TRACE("EventLoop start looping");

  while (!quit_) {
    activeChannels_.clear();
    // 阻塞等待就绪事件
    activeChannels_ = poller_->poll(kPollTimeMs);
    eventHandling_ = true;
    for (auto* channel : activeChannels_) {
      channel->handleEvent();
    }
    eventHandling_ = false;
    doPendingFunctors();
  }
  LOG_TRACE("EventLoop stop looping");
  looping_ = false;
}

void EventLoop::quit() {
  quit_ = true;
  if (!isInLoopThread()) {
    wakeup();
  }
}

void EventLoop::runInLoop(Functor cb) {
  if (isInLoopThread()) {
    //  EventLoop的单线程执行的，所以创建线程这里可以直接调用
    cb();
  } else {
    queueInLoop(std::move(cb));
  }
}

void EventLoop::queueInLoop(Functor cb) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingFunctors_.push_back(std::move(cb));
  }
  // 当为外部线程或者正在处理pending时添加唤醒事件
  if (!isInLoopThread() || callingPendingFunctors_) {
    wakeup();
  }
}

int EventLoop::createEventFd() {
  int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (evtfd < 0) {
    LOG_ERROR("Failed in eventfd");
    abort();
  }
  return evtfd;
}

void EventLoop::updateChannel(Channel* channel) {
  if (channel == nullptr) {
    return;
  }
  assertInLoopThread();
  poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel) {
  assertInLoopThread();
  poller_->removeChannel(channel);
}
void EventLoop::wakeup() {
  uint64_t one = 1;
  ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
  if (n != sizeof(one)) {
    LOG_ERROR("EventLoop::handleRead() write {} bytes instead of 8", n);
  }
}

void EventLoop::handelWakeup() {
  uint64_t one = 1;
  ssize_t n = ::read(wakeupFd_, &one, sizeof(one));
  if (n != sizeof(one)) {
    LOG_ERROR("EventLoop::handleRead() reads {} bytes instead of 8", n);
  }
}

void EventLoop::doPendingFunctors() {
  std::vector<Functor> functors;
  callingPendingFunctors_ = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    functors.swap(pendingFunctors_);
  }

  for (const auto& functor : functors) {
    functor();
  }
  callingPendingFunctors_ = false;
}


void EventLoop::assertInLoopThread() {
  if (!isInLoopThread()) {
    LOG_ERROR(
        "EventLoop::abortNotInLoopThread - EventLoop {} was created in threadId_ = {} , current "
        "thread id = {}");
  }
}

bool EventLoop::isInLoopThread() const {
  return threadId_ == std::this_thread::get_id();
}
