/*
**
* BEGIN_COPYRIGHT
*
* This file is part of SciDB.
* Copyright (C) 2008-2014 SciDB, Inc.
*
* SciDB is free software: you can redistribute it and/or modify
* it under the terms of the AFFERO GNU General Public License as published by
* the Free Software Foundation.
*
* SciDB is distributed "AS-IS" AND WITHOUT ANY WARRANTY OF ANY KIND,
* INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY,
* NON-INFRINGEMENT, OR FITNESS FOR A PARTICULAR PURPOSE. See
* the AFFERO GNU General Public License for the complete license terms.
*
* You should have received a copy of the AFFERO GNU General Public License
* along with SciDB.  If not, see <http://www.gnu.org/licenses/agpl-3.0.html>
*
* END_COPYRIGHT
*/

/*
 * Connection.h
 *
 *  Created on: Jan 15, 2010
 *      Author: roman.simakov@gmail.com
 */

#ifndef CONNECTION_H_
#define CONNECTION_H_

#include <deque>
#include <map>
#include <stdint.h>
#include <boost/asio.hpp>
#include <boost/enable_shared_from_this.hpp>

#include <util/Mutex.h>
#include <array/Metadata.h>
#include <network/proto/scidb_msg.pb.h>
#include <network/BaseConnection.h>
#include <network/NetworkManager.h>

namespace scidb
{

/**
 * Class for connect to asynchronous message exchanging between
 * network managers. It is used by network manager itself for sending message
 * to another instance and by client to connect to scidb instance.
 * @note
 * All operations are executed on the io_service::run() thread.
 * If/when multiple threads ever execute io_service::run(), io_service::strand
 * should/can be used to serialize this class execution.
 */
    class Connection: virtual public ClientContext, private BaseConnection, public boost::enable_shared_from_this<Connection>
    {
    private:
        /**
         * A FIFO message stream/channel. It is associated/identified with/by a value of type NetworkManager::MessageQueueType
         */
        class Channel
        {
        public:
            Channel(InstanceID instanceId, NetworkManager::MessageQueueType mqt)
            : _instanceId(instanceId), _mqt(mqt), _remoteSize(1),
            _localSeqNum(0), _remoteSeqNum(0), _localSeqNumOnPeer(0),
            _sendQueueLimit(1)
            {
                assert(mqt < NetworkManager::mqtMax);
                NetworkManager* networkManager = NetworkManager::getInstance();
                assert(networkManager);
                _sendQueueLimit = networkManager->getSendQueueLimit(mqt);
                _sendQueueLimit = (_sendQueueLimit>1) ? _sendQueueLimit : 1;

                _remoteSize = networkManager->getReceiveQueueHint(mqt);
                _remoteSize = (_remoteSize>1) ? _remoteSize : 1;
            }
            ~Channel() {}

            /**
             * Push a message into the tail end of the channel
             *
             * @param msg message to insert
             * @throws NetworkManager::OverflowExcetion if there is not enough space on either sender or receiver side
             * @return a new status indicating a change from the previous status,
             *         used to indicate transitions to/from the out-of-space state
             */
            boost::shared_ptr<NetworkManager::ConnectionStatus> pushBack(const boost::shared_ptr<MessageDesc>& msg);

            /**
             * Pop the next available message (if any) from the channel
             *
             * @param msg next dequeued message, which can be empty if the channel is empty or if the receiver is out of space
             * @return a new status indicating a change from the previous status,
             *         used to indicate transitions to/from the out-of-space state
             */
            boost::shared_ptr<NetworkManager::ConnectionStatus> popFront(boost::shared_ptr<MessageDesc>& msg);

            /**
             * Set the available channel space on the receiver
             *
             * @param rSize remote queue size (in number of messages for now)
             * @param localSeqNum the last sequence number generated by this instance as observed by the peer
             * @param remoteSeqNum the last sequence number generated by the peer (as observed by this instance)
             * @return a new status indicating a change from the previous status,
             *         used to indicate transitions to/from the out-of-space state
             */
            boost::shared_ptr<NetworkManager::ConnectionStatus> setRemoteState(uint64_t remoteSize,
                                                                               uint64_t localSeqNum,
                                                                               uint64_t remoteSeqNum);
            /**
             * Validate the information received from the peer
             *
             * @param rSize remote queue size (in number of messages for now)
             * @param localSeqNum the last sequence number generated by this instance as observed by the peer
             * @param remoteSeqNum the last sequence number generated by the peer (as observed by this instance)
             * @return true if peer's information is consistent with the local information; false otherwise
             */
            bool validateRemoteState(uint64_t remoteSize,
                                     uint64_t localSeqNum,
                                     uint64_t remoteSeqNum) const
            {
                return (_localSeqNum>=localSeqNum);
            }

            /// Are there messages ready to be poped ?
            bool isActive() const
            {
                assert(_localSeqNum>=_localSeqNumOnPeer);
                return ((_remoteSize > (_localSeqNum-_localSeqNumOnPeer)) && !_msgQ.empty());
            }

            /// Drop any buffered messages and abort their queries
            void abortMessages();

            boost::shared_ptr<NetworkManager::ConnectionStatus> getNewStatus(const uint64_t spaceBefore,
                                                                             const uint64_t spaceAfter);
            /// Get available space (in number of messages for now)
            uint64_t getAvailable() const ;
            uint64_t getLocalSeqNum() const
            {
                return _localSeqNum;
            }
            uint64_t getRemoteSeqNum() const
            {
                return _remoteSeqNum;
            }

        private:
            Channel();
            Channel(const Channel& other);
            Channel& operator=(const Channel& right);
        private:
            InstanceID _instanceId;
            NetworkManager::MessageQueueType _mqt;
            uint64_t _remoteSize;
            uint64_t _localSeqNum;
            uint64_t _remoteSeqNum;
            uint64_t _localSeqNumOnPeer;
            typedef std::deque<boost::shared_ptr<MessageDesc> > MessageQueue;
            MessageQueue _msgQ;
            uint64_t _sendQueueLimit;
        };

        /**
         * A message queue with multiple parallel FIFO channels/streams:
         * one channel per NetworkManager::MessageQueueType.
         * FIFO is enforced on per-channel basis.
         * The channels are drained in a round-robin fashion.
         */
        class MultiChannelQueue
        {
        public:
            MultiChannelQueue(InstanceID instanceId)
            : _instanceId(instanceId),
            _channels(NetworkManager::mqtMax),
            _currChannel(NetworkManager::mqtNone),
            _activeChannelCount(0),
            _size(0),
            _remoteGenId(0),
            _localGenId(getNextGenId()) {}
            ~MultiChannelQueue() {}

            /**
             * Append a new message to the end of the queue of a given MessageQueueType
             *
             * @param mqt message queue type to identy the appropriate channel
             * @param msg message to append
             * @throws NetworkManager::OverflowExcetion if there is not enough queue space on either sender or receiver side
             * @return a new status indicating a change from the previous status,
             *         used to indicate transitions to/from the out-of-space state
             */
            boost::shared_ptr<NetworkManager::ConnectionStatus> pushBack(NetworkManager::MessageQueueType mqt,
                                                                          const boost::shared_ptr<MessageDesc>& msg);
            /**
             * Dequeue the next available message if any.
             *
             * @param msg next dequeued message, which can be empty if the channel is empty or if the receiver is out of space
             * @return a new status indicating a change from the previous status,
             *         used to indicate transitions to/from the out-of-space state
             */
            boost::shared_ptr<NetworkManager::ConnectionStatus> popFront(boost::shared_ptr<MessageDesc>& msg);

            /**
             * Set the available queue space on the receiver
             *
             * @param mqt message queue type to identy the appropriate channel
             * @param rSize remote queue size (in number of messages for now)
             * @param localSeqNum the last sequence number generated by this instance as observed by the peer
             * @param remoteSeqNum the last sequence number generated by the peer (as observed by this instance)
             * @return a new status indicating a change from the previous status,
             *         used to indicate transitions to/from the out-of-space state
             */
            boost::shared_ptr<NetworkManager::ConnectionStatus> setRemoteState(NetworkManager::MessageQueueType mqt,
                                                                               uint64_t rSize,
                                                                               uint64_t localGenId,
                                                                               uint64_t remoteGenId,
                                                                               uint64_t localSeqNum,
                                                                               uint64_t remoteSeqNum);
            /**
             * Get available queue space on i.e. min(sender_space,receive_space)
             *
             * @param mqt message queue type to identy the appropriate channel
             * @return queue size (in number of messages for now)
             */
            uint64_t getAvailable(NetworkManager::MessageQueueType mqt) const ;

            /// Are there messages ready to be dequeued ?
            bool isActive() const
            {
                assert(_activeChannelCount<=NetworkManager::mqtMax);
                return (_activeChannelCount > 0);
            }
            uint64_t size() const
            {
                return _size;
            }
            uint64_t getLocalGenId() const
            {
                return _localGenId;
            }
            uint64_t getRemoteGenId() const
            {
                return _remoteGenId;
            }

            /// Abort enqued messages and their queries
            void abortMessages();
            void swap(MultiChannelQueue& other);

            uint64_t getLocalSeqNum(NetworkManager::MessageQueueType mqt) const ;
            uint64_t getRemoteSeqNum(NetworkManager::MessageQueueType mqt) const ;

        private:
            MultiChannelQueue();
            MultiChannelQueue(const MultiChannelQueue& other);
            MultiChannelQueue& operator=(const MultiChannelQueue& right);
        private:
            InstanceID _instanceId;
            // # of channels is not expected to be large/or changing, so vector should be OK
            typedef std::vector<boost::shared_ptr<Channel> > Channels;
            Channels _channels;
            uint32_t _currChannel;
            size_t   _activeChannelCount;
            uint64_t _size;
            uint64_t _remoteGenId;
            uint64_t _localGenId;
            static uint64_t getNextGenId()
            {
                const uint64_t billion = 1000000000;
#ifdef __APPLE__
                struct timeval tv;
                if (gettimeofday(&tv, NULL) == -1) {
                    assert(false);
                    throw SYSTEM_EXCEPTION(SCIDB_SE_INTERNAL, SCIDB_LE_CANT_GET_SYSTEM_TIME);
                }
                return tv.tv_sec*billion + tv.tv_usec*1000;
#else
                struct timespec ts;
                if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
                    assert(false);
                    throw SYSTEM_EXCEPTION(SCIDB_SE_INTERNAL, SCIDB_LE_CANT_GET_SYSTEM_TIME);
                }
                return (ts.tv_sec*billion + ts.tv_nsec);
#endif
            }
        };

      private:

        boost::shared_ptr<MessageDesc> _messageDesc;
        MultiChannelQueue _messageQueue;
        class NetworkManager& _networkManager;
        InstanceID _instanceID;
        InstanceID _sourceInstanceID;

        typedef enum
        {
            NOT_CONNECTED,
            CONNECT_IN_PROGRESS,
            CONNECTED
        } ConnectionState;

        ConnectionState _connectionState;
        boost::asio::ip::address _remoteIp;
        boost::system::error_code _error;
        boost::shared_ptr<boost::asio::ip::tcp::resolver::query> _query;
        std::map<QueryID, ClientContext::DisconnectHandler> _activeClientQueries;
        Mutex _mutex;
        bool _isSending;
        bool _logConnectErrors;
        typedef
        std::map<NetworkManager::MessageQueueType, boost::shared_ptr<const NetworkManager::ConnectionStatus> >
        ConnectionStatusMap;
        ConnectionStatusMap _statusesToPublish;

        void handleReadError(const boost::system::error_code& error);
        void onResolve(boost::shared_ptr<boost::asio::ip::tcp::resolver>& resolver,
                       boost::shared_ptr<boost::asio::ip::tcp::resolver::query>& query,
                       const boost::system::error_code& err,
                       boost::asio::ip::tcp::resolver::iterator endpoint_iterator);

        void onConnect(boost::shared_ptr<boost::asio::ip::tcp::resolver>& resolver,
                       boost::shared_ptr<boost::asio::ip::tcp::resolver::query>& query,
                       boost::asio::ip::tcp::resolver::iterator endpoint_iterator,
                       const boost::system::error_code& err);
        void disconnectInternal();
        void connectAsyncInternal(const std::string& address, uint16_t port);
        void abortMessages();

        void readMessage();
        void handleReadMessage(const boost::system::error_code&, size_t);
        void handleReadRecordPart(const boost::system::error_code&, size_t);
        void handleReadBinaryPart(const boost::system::error_code&, size_t);
        void handleSendMessage(const boost::system::error_code&, size_t,
                               boost::shared_ptr< std::list<shared_ptr<MessageDesc> > >&,
                               size_t);
        void pushNextMessage();
        std::string getPeerId();
        void getRemoteIp();
        void pushMessage(boost::shared_ptr<MessageDesc>& messageDesc,
                         NetworkManager::MessageQueueType mqt);
        boost::shared_ptr<MessageDesc> popMessage();
        bool publishQueueSizeIfNeeded(const boost::shared_ptr<const NetworkManager::ConnectionStatus>& connStatus);
        void publishQueueSize();
        boost::shared_ptr<MessageDesc> getControlMessage();

      private:
        Connection();
        Connection(const Connection& other);
        Connection& operator=(const Connection& right);

      public:
        Connection(NetworkManager& networkManager, InstanceID sourceInstanceID, InstanceID instanceID = INVALID_INSTANCE);
        virtual ~Connection();

        virtual void attachQuery(QueryID queryID, ClientContext::DisconnectHandler& dh);
        void attachQuery(QueryID queryID);
        virtual void detachQuery(QueryID queryID);

        bool isConnected() const
        {
            return  _connectionState == CONNECTED;
        }

        /// The first method executed for the incoming connected socket
        void start();
        void sendMessage(boost::shared_ptr<MessageDesc> messageDesc,
                         NetworkManager::MessageQueueType mqt = NetworkManager::mqtNone);
        /**
         * Asynchronously connect to the remote site, address:port.
         * It does not wait for the connect to complete.
         * If the connect operation fails, it is scheduled for
         * a reconnection (with the currently available address:port from SystemCatalog).
         * Connection operations can be invoked immediately after the call to connectAsync.
         * @param address[in] target DNS name or IP(v4)
         * @param port target port
         */
        void connectAsync(const std::string& address, uint16_t port);

        /**
         * Disconnect the socket and abort all in-flight async operations
         */
        virtual void disconnect();

        boost::asio::ip::tcp::socket& getSocket()
        {
            return _socket;
        }

        /// For internal use
        void setRemoteQueueState(NetworkManager::MessageQueueType mqt, uint64_t size,
                                 uint64_t localGenId,
                                 uint64_t remoteGenId,
                                 uint64_t localSn,
                                 uint64_t remoteSn);
        uint64_t getAvailable(NetworkManager::MessageQueueType mqt) const
        {
            return _messageQueue.getAvailable(mqt);
        }

        class ServerMessageDesc : public MessageDesc
        {
          public:
            ServerMessageDesc() {}
            ServerMessageDesc(boost::shared_ptr<SharedBuffer> binary) : MessageDesc(binary) {}
            virtual ~ServerMessageDesc() {}
            virtual bool validate();
          protected:
            virtual MessagePtr createRecord(MessageID messageType);
          private:
            ServerMessageDesc(const ServerMessageDesc&);
            ServerMessageDesc& operator=(const ServerMessageDesc&);
        };
    };
}

#endif /* CONNECTION_H_ */
