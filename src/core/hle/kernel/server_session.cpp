// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <tuple>

#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/client_session.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/hle_ipc.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/server_session.h"
#include "core/hle/kernel/session.h"
#include "core/hle/kernel/thread.h"

namespace Kernel {

ServerSession::ServerSession() = default;
ServerSession::~ServerSession() {
    // This destructor will be called automatically when the last ServerSession handle is closed by
    // the emulated application.

    // Decrease the port's connection count.
    if (parent->port)
        parent->port->active_sessions--;

    // TODO(Subv): Wake up all the ClientSession's waiting threads and set
    // the SendSyncRequest result to 0xC920181A.

    parent->server = nullptr;
}

ResultVal<SharedPtr<ServerSession>> ServerSession::Create(std::string name) {
    SharedPtr<ServerSession> server_session(new ServerSession);

    server_session->name = std::move(name);
    server_session->parent = nullptr;

    return MakeResult(std::move(server_session));
}

bool ServerSession::ShouldWait(Thread* thread) const {
    // Closed sessions should never wait, an error will be returned from svcReplyAndReceive.
    if (parent->client == nullptr)
        return false;
    // Wait if we have no pending requests, or if we're currently handling a request.
    return pending_requesting_threads.empty() || currently_handling != nullptr;
}

void ServerSession::Acquire(Thread* thread) {
    ASSERT_MSG(!ShouldWait(thread), "object unavailable!");
    // We are now handling a request, pop it from the stack.
    // TODO(Subv): What happens if the client endpoint is closed before any requests are made?
    ASSERT(!pending_requesting_threads.empty());
    currently_handling = pending_requesting_threads.back();
    pending_requesting_threads.pop_back();
}

ResultCode ServerSession::HandleSyncRequest(SharedPtr<Thread> thread) {
    // The ServerSession received a sync request, this means that there's new data available
    // from its ClientSession, so wake up any threads that may be waiting on a svcReplyAndReceive or
    // similar.

    Kernel::HLERequestContext context(this);
    u32* cmd_buf = (u32*)Memory::GetPointer(thread->GetTLSAddress());
    context.PopulateFromIncomingCommandBuffer(cmd_buf, *Kernel::g_current_process,
                                              Kernel::g_handle_table);

    // If the session has been converted to a domain, handle the doomain request
    if (IsDomain()) {
        auto& domain_message_header = context.GetDomainMessageHeader();
        if (domain_message_header) {
            // If there is a DomainMessageHeader, then this is CommandType "Request"
            const u32 object_id{context.GetDomainMessageHeader()->object_id};
            switch (domain_message_header->command) {
            case IPC::DomainMessageHeader::CommandType::SendMessage:
                return domain_request_handlers[object_id - 1]->HandleSyncRequest(context);

            case IPC::DomainMessageHeader::CommandType::CloseVirtualHandle: {
                LOG_DEBUG(IPC, "CloseVirtualHandle, object_id=0x%08X", object_id);

                domain_request_handlers[object_id - 1] = nullptr;

                IPC::ResponseBuilder rb{context, 2};
                rb.Push(RESULT_SUCCESS);
                return RESULT_SUCCESS;
            }
            }

            LOG_CRITICAL(IPC, "Unknown domain command=%d", domain_message_header->command.Value());
            ASSERT(false);
        }
        // If there is no domain header, the regular session handler is used
    }

    // If this ServerSession has an associated HLE handler, forward the request to it.
    ResultCode result{RESULT_SUCCESS};
    if (hle_handler != nullptr) {
        // Attempt to translate the incoming request's command buffer.
        ResultCode translate_result = TranslateHLERequest(this);
        if (translate_result.IsError())
            return translate_result;

        result = hle_handler->HandleSyncRequest(context);
    } else {
        // Add the thread to the list of threads that have issued a sync request with this
        // server.
        pending_requesting_threads.push_back(std::move(thread));
    }

    // If this ServerSession does not have an HLE implementation, just wake up the threads waiting
    // on it.
    WakeupAllWaitingThreads();

    // Handle scenario when ConvertToDomain command was issued, as we must do the conversion at the
    // end of the command such that only commands following this one are handled as domains
    if (convert_to_domain) {
        ASSERT_MSG(domain_request_handlers.empty(), "already a domain");
        domain_request_handlers = {hle_handler};
        convert_to_domain = false;
    }

    return result;
}

ServerSession::SessionPair ServerSession::CreateSessionPair(const std::string& name,
                                                            SharedPtr<ClientPort> port) {
    auto server_session = ServerSession::Create(name + "_Server").Unwrap();
    SharedPtr<ClientSession> client_session(new ClientSession);
    client_session->name = name + "_Client";

    std::shared_ptr<Session> parent(new Session);
    parent->client = client_session.get();
    parent->server = server_session.get();
    parent->port = port;

    client_session->parent = parent;
    server_session->parent = parent;

    return std::make_tuple(std::move(server_session), std::move(client_session));
}

ResultCode TranslateHLERequest(ServerSession* server_session) {
    // TODO(Subv): Implement this function once multiple concurrent processes are supported.
    return RESULT_SUCCESS;
}
} // namespace Kernel
