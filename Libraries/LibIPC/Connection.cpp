/*
 * Copyright (c) 2021-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibCore/Socket.h>
#include <LibCore/Timer.h>
#include <LibIPC/Connection.h>
#include <LibIPC/Message.h>
#include <LibIPC/Stub.h>

namespace IPC {

ConnectionBase::ConnectionBase(IPC::Stub& local_stub, Transport transport, u32 local_endpoint_magic, u32 peer_endpoint_magic)
    : m_local_stub(local_stub)
    , m_transport(move(transport))
    , m_local_endpoint_magic(local_endpoint_magic)
    , m_peer_endpoint_magic(peer_endpoint_magic)
{
    m_responsiveness_timer = Core::Timer::create_single_shot(3000, [this] { may_have_become_unresponsive(); });

    m_transport.set_up_read_hook([this] {
        NonnullRefPtr protect = *this;
        // FIXME: Do something about errors.
        (void)drain_messages_from_peer();
        handle_messages();
    });

    m_send_queue = adopt_ref(*new SendQueue);
    m_acknowledgement_wait_queue = adopt_ref(*new AcknowledgementWaitQueue);
    m_send_thread = Threading::Thread::construct([this, send_queue = m_send_queue, acknowledgement_wait_queue = m_acknowledgement_wait_queue]() -> intptr_t {
        for (;;) {
            send_queue->mutex.lock();
            while (send_queue->messages.is_empty() && send_queue->running)
                send_queue->condition.wait();

            if (!send_queue->running) {
                send_queue->mutex.unlock();
                break;
            }

            auto [message_buffer, needs_acknowledgement] = send_queue->messages.take_first();
            send_queue->mutex.unlock();

            if (needs_acknowledgement == MessageNeedsAcknowledgement::Yes) {
                Threading::MutexLocker lock(acknowledgement_wait_queue->mutex);
                acknowledgement_wait_queue->messages.append(message_buffer);
            }

            if (auto result = message_buffer.transfer_message(m_transport); result.is_error()) {
                dbgln("ConnectionBase::send_thread: {}", result.error());
                continue;
            }
        }
        return 0;
    });
    m_send_thread->start();
}

ConnectionBase::~ConnectionBase()
{
    {
        Threading::MutexLocker locker(m_send_queue->mutex);
        m_send_queue->running = false;
        m_send_queue->condition.signal();
    }
    m_send_thread->detach();
}

bool ConnectionBase::is_open() const
{
    return m_transport.is_open();
}

ErrorOr<void> ConnectionBase::post_message(Message const& message)
{
    return post_message(message.endpoint_magic(), TRY(message.encode()));
}

ErrorOr<void> ConnectionBase::post_message(u32 endpoint_magic, MessageBuffer buffer, MessageNeedsAcknowledgement needs_acknowledgement)
{
    // NOTE: If this connection is being shut down, but has not yet been destroyed,
    //       the socket will be closed. Don't try to send more messages.
    if (!m_transport.is_open())
        return Error::from_string_literal("Trying to post_message during IPC shutdown");

    if (buffer.data().size() > TransportSocket::SOCKET_BUFFER_SIZE) {
        auto wrapper = LargeMessageWrapper::create(endpoint_magic, buffer);
        buffer = MUST(wrapper->encode());
    }

    {
        Threading::MutexLocker locker(m_send_queue->mutex);
        m_send_queue->messages.append({ move(buffer), needs_acknowledgement });
        m_send_queue->condition.signal();
    }

    m_responsiveness_timer->start();
    return {};
}

void ConnectionBase::shutdown()
{
    m_transport.close();
    die();
}

void ConnectionBase::shutdown_with_error(Error const& error)
{
    dbgln("IPC::ConnectionBase ({:p}) had an error ({}), disconnecting.", this, error);
    shutdown();
}

void ConnectionBase::handle_messages()
{
    auto messages = move(m_unprocessed_messages);
    for (auto& message : messages) {
        if (message->endpoint_magic() == m_local_endpoint_magic) {
            auto handler_result = m_local_stub.handle(move(message));
            if (handler_result.is_error()) {
                dbgln("IPC::ConnectionBase::handle_messages: {}", handler_result.error());
                continue;
            }

            if (auto response = handler_result.release_value()) {
                if (auto post_result = post_message(m_local_endpoint_magic, *response); post_result.is_error()) {
                    dbgln("IPC::ConnectionBase::handle_messages: {}", post_result.error());
                }
            }
        }
    }
}

void ConnectionBase::wait_for_transport_to_become_readable()
{
    m_transport.wait_until_readable();
}

ErrorOr<Vector<u8>> ConnectionBase::read_as_much_as_possible_from_transport_without_blocking()
{
    Vector<u8> bytes;

    if (!m_unprocessed_bytes.is_empty()) {
        bytes.append(m_unprocessed_bytes.data(), m_unprocessed_bytes.size());
        m_unprocessed_bytes.clear();
    }

    bool should_shut_down = false;
    auto schedule_shutdown = [this, &should_shut_down]() {
        should_shut_down = true;
        deferred_invoke([this] {
            shutdown();
        });
    };

    auto&& [new_bytes, received_fds] = m_transport.read_as_much_as_possible_without_blocking(move(schedule_shutdown));
    bytes.append(new_bytes.data(), new_bytes.size());

    for (auto const& fd : received_fds)
        m_unprocessed_fds.enqueue(IPC::File::adopt_fd(fd));

    if (!bytes.is_empty()) {
        m_responsiveness_timer->stop();
        did_become_responsive();
    } else if (should_shut_down) {
        return Error::from_string_literal("IPC connection EOF");
    }

    return bytes;
}

ErrorOr<void> ConnectionBase::drain_messages_from_peer()
{
    auto bytes = TRY(read_as_much_as_possible_from_transport_without_blocking());

    size_t index = 0;
    try_parse_messages(bytes, index);

    if (index < bytes.size()) {
        // Sometimes we might receive a partial message. That's okay, just stash away
        // the unprocessed bytes and we'll prepend them to the next incoming message
        // in the next run of this function.
        auto remaining_bytes = TRY(ByteBuffer::copy(bytes.span().slice(index)));
        if (!m_unprocessed_bytes.is_empty()) {
            shutdown();
            return Error::from_string_literal("drain_messages_from_peer: Already have unprocessed bytes");
        }
        m_unprocessed_bytes = move(remaining_bytes);
    }

    if (!m_unprocessed_messages.is_empty()) {
        deferred_invoke([this] {
            handle_messages();
        });
    }
    return {};
}

OwnPtr<IPC::Message> ConnectionBase::wait_for_specific_endpoint_message_impl(u32 endpoint_magic, int message_id)
{
    for (;;) {
        // Double check we don't already have the event waiting for us.
        // Otherwise we might end up blocked for a while for no reason.
        for (size_t i = 0; i < m_unprocessed_messages.size(); ++i) {
            auto& message = m_unprocessed_messages[i];
            if (message->endpoint_magic() != endpoint_magic)
                continue;
            if (message->message_id() == message_id)
                return m_unprocessed_messages.take(i);
        }

        if (!is_open())
            break;

        wait_for_transport_to_become_readable();
        if (drain_messages_from_peer().is_error())
            break;
    }
    return {};
}

void ConnectionBase::try_parse_messages(Vector<u8> const& bytes, size_t& index)
{
    u32 message_size = 0;
    u32 pending_ack_count = 0;
    u32 received_ack_count = 0;
    for (; index + sizeof(message_size) < bytes.size(); index += message_size) {
        memcpy(&message_size, bytes.data() + index, sizeof(message_size));
        if (message_size == 0 || bytes.size() - index - sizeof(uint32_t) < message_size)
            break;
        index += sizeof(message_size);
        auto remaining_bytes = ReadonlyBytes { bytes.data() + index, message_size };

        if (auto message = try_parse_message(remaining_bytes, m_unprocessed_fds)) {
            if (message->message_id() == LargeMessageWrapper::MESSAGE_ID) {
                LargeMessageWrapper* wrapper = static_cast<LargeMessageWrapper*>(message.ptr());
                auto wrapped_message = wrapper->wrapped_message_data();
                m_unprocessed_fds.return_fds_to_front_of_queue(wrapper->take_fds());
                auto parsed_message = try_parse_message(wrapped_message, m_unprocessed_fds);
                VERIFY(parsed_message);
                VERIFY(parsed_message->message_id() != Acknowledgement::MESSAGE_ID);
                pending_ack_count++;
                m_unprocessed_messages.append(parsed_message.release_nonnull());
                continue;
            }

            if (message->message_id() == Acknowledgement::MESSAGE_ID) {
                VERIFY(message->endpoint_magic() == m_local_endpoint_magic);
                received_ack_count += static_cast<Acknowledgement*>(message.ptr())->ack_count();
                continue;
            }

            pending_ack_count++;
            m_unprocessed_messages.append(message.release_nonnull());
            continue;
        }

        dbgln("Failed to parse IPC message:");
        dbgln("{:hex-dump}", remaining_bytes);
        break;
    }

    if (received_ack_count > 0) {
        Threading::MutexLocker lock(m_acknowledgement_wait_queue->mutex);
        for (size_t i = 0; i < received_ack_count; ++i)
            m_acknowledgement_wait_queue->messages.take_first();
    }

    if (is_open() && pending_ack_count > 0) {
        auto acknowledgement = Acknowledgement::create(m_peer_endpoint_magic, pending_ack_count);
        MUST(post_message(m_peer_endpoint_magic, MUST(acknowledgement->encode()), MessageNeedsAcknowledgement::No));
    }
}

}
