#include "server/netclientsession.h"

#include "server/messagesender.h"

#include "persistence/datastreamobserver.h"
#include "persistence/json/jsonserializer.h"
#include "persistence/net/jsonserializer.h"

namespace server
{

  namespace detail
  {
    class SessionStreamObserver final : public persistence::DataStreamObserver
    {
    public:
      SessionStreamObserver(MessageSender& sender, int clientStreamId)
          : _sender(sender), _clientStreamId(clientStreamId)
      {
      }
      virtual ~SessionStreamObserver() {}
      int clientStreamId() { return _clientStreamId; }

      virtual void addItems(const persistence::StreamableItems& items) override
      {
        _sender.sendMessage(persistence::net::JsonSerializer::serializeStreamAddMessage(_clientStreamId, items));
      }

      virtual void updateItems(const persistence::StreamableItems& items) override
      {
        _sender.sendMessage(persistence::net::JsonSerializer::serializeStreamUpdateMessage(_clientStreamId, items));
      }

      virtual void removeItems(const std::vector<int>& ids) override
      {
        _sender.sendMessage(persistence::net::JsonSerializer::serializeStreamRemoveMessage(_clientStreamId, ids));
      }

      virtual void clear() override
      {
        _sender.sendMessage(persistence::net::JsonSerializer::serializeStreamClearMessage(_clientStreamId));
      }

      virtual void initialized() override
      {
        _sender.sendMessage(persistence::net::JsonSerializer::serializeStreamInitializeMessage(_clientStreamId));
      }

    private:
      MessageSender& _sender;
      int _clientStreamId;
    };

    /**
     * @brief The SessionTaskObserver class listens to changes of a single tasks and reports them back to the server
     */
    class SessionTaskObserver final : public persistence::TaskObserver
    {
    public:
      SessionTaskObserver(MessageSender& sender, int clientTaskId, persistence::Backend& backend,
                          persistence::op::Operations ops)
          : _sender(sender), _clientTaskId(clientTaskId), _handle()
      {
        _handle = backend.queueOperations(std::move(ops), this);
      }

    private:
      virtual void setResults(const std::vector<persistence::TaskResult>& results) override
      {
        auto message = persistence::net::JsonSerializer::serializeTaskResultsMessage(_clientTaskId, results);
        _sender.sendMessage(message);
      }

      MessageSender& _sender;
      int _clientTaskId;
      persistence::UniqueTaskHandle _handle;
    };

  }  // namespace detail

  NetClientSession::NetClientSession(boost::asio::io_service& ioService, persistence::Backend &backend)
      : _backend(backend), _socket(ioService)
  {
  }

  NetClientSession::~NetClientSession()
  {
    if (_socket.is_open())
      std::cout << " [-] Client disconnected " << _socket.remote_endpoint().address().to_string() << std::endl;
  }

  void NetClientSession::start()
  {
    doReadHeader();
  }

  void NetClientSession::close()
  {
    if (_socket.is_open())
      _socket.close();
  }

  void NetClientSession::sendMessage(const nlohmann::json& json)
  {
    bool sendInProgress = !_outgoingMessages.empty();
    _outgoingMessages.push(json.dump());
    if (!sendInProgress)
      doSend();
  }

  void NetClientSession::doReadHeader()
  {
    _socket.async_read_some(boost::asio::buffer(_headerData.data(), _headerData.size()),
                            [session=this->shared_from_this()](const boost::system::error_code &ec, size_t length)
    {
      if (!ec)
      {
        session->doReadBody();
      }
    });
  }

  void NetClientSession::doReadBody()
  {
    size_t size = static_cast<size_t>(static_cast<unsigned char>(_headerData[0])) <<  0 |
                                                                                      static_cast<size_t>(static_cast<unsigned char>(_headerData[1])) <<  8 |
                                                                                                                                                          static_cast<size_t>(static_cast<unsigned char>(_headerData[2])) << 16 |
                                                                                                                                                                                                                             static_cast<size_t>(static_cast<unsigned char>(_headerData[3])) << 24;
    _bodyData.resize(size, 0);
    _socket.async_read_some(boost::asio::buffer(_bodyData.data(), _bodyData.size()),
                            [session=this->shared_from_this()](const boost::system::error_code &ec, size_t length)
    {
      if (!ec)
      {
        std::string message(session->_bodyData.data(), session->_bodyData.size());
        session->runCommand(nlohmann::json::parse(message));
        session->doReadHeader();
      }
    });
  }

  void NetClientSession::doSend()
  {
    auto message = _outgoingMessages.front();
    std::cout << " [W] " << nlohmann::json::parse(message)["op"] << " " << message.size() << " bytes" << std::endl;
    std::vector<char> data(message.size() + 4, 0);
    auto size = message.size();
    data[0] = (size >> 0) & 0xff;
    data[1] = (size >> 8) & 0xff;
    data[2] = (size >> 16) & 0xff;
    data[3] = (size >> 24) & 0xff;
    memcpy(data.data() + 4, message.data(), size);

    _socket.async_write_some(boost::asio::buffer(data.data(), data.size()),
                             [session=this->shared_from_this()](const boost::system::error_code &ec, size_t length){
      if (!ec)
      {
        session->_outgoingMessages.pop();
        if (!session->_outgoingMessages.empty())
          session->doSend();
      }
    });
  }

  void NetClientSession::runCommand(const nlohmann::json &obj)
  {
    std::string operation = obj["op"];

    if (operation == "create_stream")
      runCreateStream(obj);
    else if (operation == "remove_stream")
      runRemoveStream(obj);
    else if (operation == "schedule_operations")
      runScheduleOperations(obj);
    else
      std::cout << " [!] Unknown operation: " << operation << std::endl;
  }

  void NetClientSession::runCreateStream(const nlohmann::json &obj)
  {
    int clientId = obj["id"];
    auto type = static_cast<persistence::StreamableType>((int)obj["type"]);
    auto observer = std::make_unique<detail::SessionStreamObserver>(*this, clientId);
    auto streamHandle = _backend.createStream(observer.get(), type, obj["service"], obj["options"]);
    int serverId = streamHandle.stream()->streamId();
    std::cout << " [R] Create stream s[" << serverId << "] => c[" << clientId << "]" << std::endl;
    _streams.emplace_back(std::move(streamHandle), std::move(observer));
  }

  void NetClientSession::runRemoveStream(const nlohmann::json &obj)
  {
    int clientId = obj["id"];
    auto it = std::find_if(_streams.begin(), _streams.end(), [clientId](const auto& pair) {
      return pair.second->clientStreamId() == clientId;
    });

    if (it != _streams.end())
    {
      std::cout << " [R] Removed stream s[" << it->first.stream()->streamId() << "] => c[" << it->second->clientStreamId() << "]" << std::endl;
      _streams.erase(it);
    }
  }

  void NetClientSession::runScheduleOperations(const nlohmann::json &obj)
  {
    std::cout << " [R] Schedule " << obj["operations"].size() << " operation(s)" << std::endl;

    persistence::op::Operations operations;
    for (auto& operation : obj["operations"])
    {
      auto opType = operation["op"];
      std::cout << "     " << opType << std::endl;
      if (opType == "store_new_reservation")
      {
        auto reservation = persistence::json::deserialize<hotel::Reservation>(operation["o"]);
        operations.push_back(persistence::op::StoreNewReservation{std::make_unique<hotel::Reservation>(std::move(reservation))});
      }
      else if (opType == "update_reservation")
      {
        auto reservation = persistence::json::deserialize<hotel::Reservation>(operation["o"]);
        operations.push_back(persistence::op::UpdateReservation{std::make_unique<hotel::Reservation>(std::move(reservation))});
      }
      else if (opType == "delete_reservation")
      {
        int id = operation["o"];
        operations.push_back(persistence::op::DeleteReservation{id});
      }
      else
        std::cout << " [!] Unknown operation " << opType << ": " << operation << std::endl;
    }

    auto observer = std::make_unique<detail::SessionTaskObserver>(*this, obj["id"], _backend, std::move(operations));
    _taskObservers.push_back(std::move(observer));
  }

} // namespace server