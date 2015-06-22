#include <iostream>
#include <functional>
#include <vector>
#include <map>
#include <string>
#include <cassert>

#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace std;
using Callback = std::function<void(void)>;

class PipeListener
{
public:
  virtual ~PipeListener() {}
  virtual void OnData(const char* buffer, size_t msgLen) = 0;
};

class EventEngine
{
  // sit in a loop listening to a message queue
private:
  std::vector<Callback> mCallbacks;
  std::map<int, PipeListener*> mPipeListeners;

public:
  void Post(Callback& callback)
  {
    mCallbacks.push_back(callback);
  }

  std::pair<int, int> RegisterPipeListener(const std::string queueName, PipeListener& listener)
  {
    if (mkfifo(queueName.c_str(), S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH) == -1)
    {
      if (errno != EEXIST)
        throw std::runtime_error(strerror(errno));
    }
    int readFd;
    if ((readFd = open(queueName.c_str(), O_RDONLY | O_NONBLOCK)) < 0)
      throw std::runtime_error(strerror(errno));
    mPipeListeners[readFd] = &listener;
    int writeFd;
    if ((writeFd = open(queueName.c_str(), O_WRONLY | O_NONBLOCK)) < 0)
      throw std::runtime_error(strerror(errno));
    return std::make_pair(readFd, writeFd);
  }

  void Start()
  {
    while (true)
    {
      for (auto& kv : mPipeListeners)
      {
        char buf[2];

        // Form descriptor
        fd_set readset;
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        FD_ZERO(&readset);
        FD_SET(kv.first, &readset);
        int available = select(kv.first + 1, &readset, NULL, NULL, &tv);
        if (available == -1)
          throw std::runtime_error("Select failed");
        if (available > 0 && FD_ISSET(kv.first, &readset))
        {
          int n = read(kv.first, buf, sizeof(buf));
          if (n == 2)
          {
            int command = atoi(buf);
            switch (command)
            {
            case 0:
              kv.second->OnData("0", 2);
              break;
            case 1:
              kv.second->OnData("1", 2);
              break;
            case 2:
              kv.second->OnData("2", 2);
              break;
            default:
              throw std::runtime_error("Unknown pipe command");
              break;
            }
          }
        }
      }

      // also clear callback queue
      for (auto& callback : mCallbacks)
      {
        callback();
      }
      mCallbacks.clear();
    }
  }
};

struct OrderInsertMsg
{
  int mTag = 0;
  double mPrice = 0;
  int mQty = 0;
};

class ExecModule : private PipeListener
{
private:
  EventEngine& mEventEngine;

  // for now, only one callback at a time
  std::function<void(void)> mInsertCallback;
  std::function<void(void)> mFillCallback;

  int mWriteFd;
public:
  ExecModule(EventEngine& eventEngine) :
    mEventEngine(eventEngine)
  {
    mWriteFd = mEventEngine.RegisterPipeListener("/tmp/ExecModule", *this).second;
  }

  ~ExecModule()
  {
    if (mWriteFd >= 0)
    {
      close(mWriteFd);
    }
  }

  void SendInsert(const OrderInsertMsg& orderInsertMsg, std::function<void(int)> replyCallback, std::function<void(int)> fillCallback)
  {
    std::cout << "Exec module: sending insert to market\n";

    // record the callbacks with the given order tag
    mInsertCallback = std::bind(replyCallback, orderInsertMsg.mTag);
    mFillCallback = std::bind(fillCallback, orderInsertMsg.mTag);

    // simulate exchange reply (will be picked up in next cycle)
    if (write(mWriteFd, "1", 2) < 2)
    {
      throw std::runtime_error("Failed to write to named pipe");
    }
  }

  void OnData(const char* data, size_t msgLen) override
  {
    assert(msgLen == 2);
    int command = atoi(data);
    switch (command)
    {
      case 1:
        std::cout << "Exec module: received exchange reply, calling back sender\n";
        mEventEngine.Post(mInsertCallback);
        break;
      case 2:
        std::cout << "Exec module: received exchange fill, calling back sender\n";
        mEventEngine.Post(mFillCallback);
        break;
      default:
        throw std::runtime_error("Unknown pipe command");
      }
  }
};

class OrderManager : private PipeListener
{
private:
  EventEngine& mEventEngine;
  ExecModule mExecModule;

  enum class OrderState
  {
    New,
    Inserted,
    Filled
  };

  struct Order
  {
    int mTag = 0;
    double mPrice = 0;
    int mQty = 0;
    OrderState mOrderState = OrderState::New;
  };

  std::map<int, Order> mOrders;

  void HandleInsertOrder(const OrderInsertMsg& orderInsertMsg)
  {
    std::cout << "Order manager: received order insert message\n";
    mOrders.insert(std::make_pair(orderInsertMsg.mTag, Order()));

    // TODO now check price collars

    auto fillCallback = [this](int tag)
    {
      std::cout << "Order manager: received fill\n";

      // update order state again
      auto& order = mOrders[tag];
      order.mOrderState = OrderState::Filled;
      // TODO slow thread work
    };
    mExecModule.SendInsert(orderInsertMsg, [this](int tag)
    {
        std::cout << "Order manager: received order reply\n";

        // update order state again now that we have been called back
        auto& order = mOrders[tag];
        order.mOrderState = OrderState::Inserted;
        // TODO other slow thread things here (i.e. push into slow thread message queue)
    }, fillCallback);
    // TODO pubsub out a message that we have an order insert, but do this on the slow thread, by passing a message to slow thread
  }

public:
  OrderManager(EventEngine& eventEngine) :
    mEventEngine(eventEngine),
    mExecModule(mEventEngine)
  {
    mEventEngine.RegisterPipeListener("/tmp/OrderManager", *this);
  }

  void OnData(const char* data, size_t msgLen) override
  {
    // assume that this is an order insert message
    HandleInsertOrder(OrderInsertMsg());
  }
};

// TODO complete this and make it thread safe
class MessageQueue
{
  void WriteMessage()
  {
  }
  void ReadMessage()
  {
  }
};

int main()
{
  MessageQueue messageQueue;
  EventEngine eventEngine;
  OrderManager orderManager(eventEngine);
  eventEngine.Start();

  // TODO have a second thread which does the slow work (writes to output pipe, etc), including a single writer, single reader message queue
  // TODO handle price collars - write to ping pong buffer, and then inspect (generate randomly from slow thread)
  // TODO replace std functions with lambdas
  // TODO implement own callable queue?
}

