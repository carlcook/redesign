#include <iostream>
#include <functional>
#include <vector>
#include <map>
#include <string>

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

  void RegisterPipeListener(const std::string queueName, PipeListener& listener)
  {
    // TODO open fifo pipe and use the given fd
    mPipeListeners[0] = &listener;
  }

  void Start()
  {
    while (true)
    {
      // listen to pipes for data
      // if it comes, call correct listener

      // also clear callback queue
      for (auto& callback : mCallbacks)
      {
        callback();
      }
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
  EventEngine mEventEngine;

  // for now, only one callback at a time
  std::function<void(void)> mInsertCallback;
public:
  ExecModule(EventEngine& eventEngine) :
    mEventEngine(eventEngine)
  {
    // TODO create fifo pipe here
    mEventEngine.RegisterPipeListener("ExecModule", *this);
  }

  void SendInsert(const OrderInsertMsg& orderInsertMsg, std::function<void(int)> callback)
  {
    // record the callback with the given order tag
    mInsertCallback = std::bind(callback, orderInsertMsg.mTag);
    // TODO write to the named pipe, simulating an exchange reply
  }

  void OnData(const char* data, size_t msgLen) override
  {
    // TODO either an insert reply or a fill message
    mEventEngine.Post(mInsertCallback);
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
    Deleted
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
    mOrders.insert(std::make_pair(orderInsertMsg.mTag, Order()));
    mExecModule.SendInsert(orderInsertMsg, [this](int tag)
    {
        // update order state again now that we have been called back
        auto& order = mOrders[tag];
        order.mOrderState = OrderState::Inserted;
        // TODO other slow thread things here (i.e. push into slow thread message queue)
    });
    // TODO pubsub out a message that we have an order insert, but do this on the slow thread
  }

public:
  OrderManager(EventEngine& eventEngine) :
    mEventEngine(eventEngine),
    mExecModule(mEventEngine)
  {
    // TODO create fifo pipe here
    mEventEngine.RegisterPipeListener("OrderManager", *this);
  }

  void OnData(const char* data, size_t msgLen) override
  {
    // event engine thread
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
}

