#ifndef PERSISTENCE_DATASTREAM_H
#define PERSISTENCE_DATASTREAM_H

#include "persistence/datastreamobserver.h"

#include "boost/variant.hpp"

#include "extern/nlohmann_json/json.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace persistence
{
  /**
   * @brief The StreamableType enum holds all possible native data types a steam can have
   */
  enum class StreamableType { NullStream, Hotel, Reservation };

  struct DataStreamItemsAdded { StreamableItems newItems; };
  struct DataStreamItemsRemoved { std::vector<int> removedItems; };
  struct DataStreamInitialized {};
  struct DataStreamCleared {};
  typedef boost::variant<DataStreamItemsAdded,
                         DataStreamItemsRemoved,
                         DataStreamInitialized,
                         DataStreamCleared>
          DataStreamChange;

  /**
   * @brief Writable backend for data stream
   */
  class DataStream
  {
  public:
    DataStream(StreamableType streamType, const std::string& endpoint, const nlohmann::json& options)
        : _streamId(0), _streamType(streamType), _endpoint(endpoint), _options(options), _isInitialized(false),
          _observer(nullptr)
    {
    }
    virtual ~DataStream() {}

    void connect(int streamId, DataStreamObserver* observer)
    {
      assert(_streamId == 0);
      assert(_observer == nullptr);
      _streamId = streamId;
      _observer = observer;
    }

    //! Returns the unique ID of this stream
    int streamId() const { return _streamId; }
    //! Returns the datatype of this stream
    StreamableType streamType() const { return _streamType; }

    const std::string& streamEndpoint() const { return _endpoint; }
    const nlohmann::json& streamOptions() const { return _options; }
    //! Returns true if there is still an observer listening on this stream
    bool isValid() const { return _observer != nullptr; }
    //! Returns true if the initial data for the observer has already been set
    bool isInitialized() const { return _isInitialized; }
    //! Dissociates the stream from the observer.
    void disconnect() { _observer = nullptr; }

    void applyChange(DataStreamChange change)
    {
      if (_observer)
        boost::apply_visitor([this](auto& change){
          this->applyChange(change);
        }, change);
    }

    template <class T>
    static StreamableType GetStreamTypeFor();

  private:
    void applyChange(const DataStreamItemsAdded& op) { _observer->addItems(op.newItems); }
    void applyChange(const DataStreamItemsRemoved& op) { _observer->removeItems(op.removedItems); }
    void applyChange(const DataStreamInitialized& op) { _isInitialized = true; _observer->initialized(); }
    void applyChange(const DataStreamCleared& op) { _observer->clear(); }

    int _streamId;
    StreamableType _streamType;
    std::string _endpoint;
    nlohmann::json _options;
    bool _isInitialized;
    DataStreamObserver* _observer;
  };

  /**
   * @brief The UniqueDataStreamHandle class is a handle for a data stream connection
   *
   * When this handle is destroyed, the associated data stream observer will no longer get any change notifications.
   */
  class UniqueDataStreamHandle
  {
  public:
    UniqueDataStreamHandle()
        : _dataStream(nullptr)
    {}
    UniqueDataStreamHandle(std::shared_ptr<DataStream> dataStream)
        : _dataStream(dataStream)
    {}

    UniqueDataStreamHandle(const UniqueDataStreamHandle& that) = delete;
    UniqueDataStreamHandle& operator=(const UniqueDataStreamHandle& that) = delete;

    UniqueDataStreamHandle(UniqueDataStreamHandle&& that) = default;
    UniqueDataStreamHandle& operator=(UniqueDataStreamHandle&& that) = default;
    ~UniqueDataStreamHandle() { if (_dataStream) _dataStream->disconnect(); }

  private:
    std::shared_ptr<DataStream> _dataStream;
  };

}

#endif // PERSISTENCE_DATASTREAM_H
