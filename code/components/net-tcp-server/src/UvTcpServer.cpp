/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include "UvTcpServer.h"
#include "TcpServerManager.h"
#include "UvLoopManager.h"
#include "memdbgon.h"

#include <botan/auto_rng.h>

#ifdef _WIN32
#pragma comment(lib, "ntdll")
#include <winternl.h>

EXTERN_C NTSTATUS NTSYSAPI NTAPI NtSetInformationFile(
	_In_  HANDLE                 FileHandle,
	_Out_ PIO_STATUS_BLOCK       IoStatusBlock,
	_In_  PVOID                  FileInformation,
	_In_  ULONG                  Length,
	_In_  FILE_INFORMATION_CLASS FileInformationClass
);

struct FILE_COMPLETION_INFORMATION
{
	DWORD_PTR Port;
	DWORD_PTR Key;
};

#define FileReplaceCompletionInformation (FILE_INFORMATION_CLASS)61

#define STATUS_INVALID_INFO_CLASS 0xC0000003

#endif

namespace net
{
UvTcpServer::UvTcpServer(TcpServerManager* manager)
	: m_manager(manager), m_dispatchIndex(0), m_tryDetachFromIOCP(true)
{
	m_pipeName = fmt::sprintf(
#ifdef _WIN32
		"\\\\.\\pipe\\fxserver_%d%d",
#else
		"/tmp/fxserver_%d%d",
#endif
		time(NULL), rand()
	);
	
	Botan::AutoSeeded_RNG rng;
	rng.randomize(m_helloMessage.data(), m_helloMessage.size());
}

UvTcpServer::~UvTcpServer()
{
	m_dispatchPipes.clear();
	m_createdPipes.clear();

	m_listenPipe = {};

	auto server = m_server;

	if (server)
	{
		server->close();
		m_server = {};
	}
}

bool UvTcpServer::Listen(std::shared_ptr<uvw::TCPHandle>&& server)
{
	m_server = std::move(server);

	m_server->on<uvw::ListenEvent>([this](const uvw::ListenEvent& event, uvw::TCPHandle& handle)
	{
		OnConnection(0);
	});

	m_server->on<uvw::ErrorEvent>([this](const uvw::ErrorEvent& event, uvw::TCPHandle& handle)
	{
		trace("Listening on socket failed - libuv error %s.\n", event.name());

		OnConnection(event.code());
	});

	m_server->listen();

	auto pipe = m_server->loop().resource<uvw::PipeHandle>();
	pipe->bind(m_pipeName);
	pipe->listen();

	pipe->on<uvw::ListenEvent>([this](const uvw::ListenEvent& event, uvw::PipeHandle& handle)
	{
		OnListenPipe(handle);
	});

	m_listenPipe = pipe;

	auto threadCount = std::min(std::max(int(std::thread::hardware_concurrency() / 2), 1), 8);

	for (int i = 0; i < threadCount; i++)
	{
		auto cs = std::make_shared<UvTcpChildServer>(this, m_pipeName, m_helloMessage, i);
		cs->Listen();

		m_childServers.insert(cs);
	}

	return true;
}

void UvTcpServer::OnListenPipe(uvw::PipeHandle& handle)
{
	auto pipe = m_server->loop().resource<uvw::PipeHandle>(true);
	std::weak_ptr<uvw::PipeHandle> pipeWeak(pipe);

	pipe->on<uvw::DataEvent>([this, pipeWeak](const uvw::DataEvent& event, uvw::PipeHandle& handle)
	{
		if (event.length == 0)
		{
			m_createdPipes.erase(pipeWeak.lock());
			return;
		}

		// #TODOTCP: handle partial reads
		if (event.length != m_helloMessage.size())
		{
			m_createdPipes.erase(pipeWeak.lock());
			return;
		}

		auto pipe = pipeWeak.lock();

		if (pipe)
		{
			if (memcmp(event.data.get(), m_helloMessage.data(), m_helloMessage.size()) != 0)
			{
				m_createdPipes.erase(pipe);
				return;
			}

			m_dispatchPipes.push_back(pipe);
		}
	});

	pipe->on<uvw::EndEvent>([this, pipeWeak](const uvw::EndEvent& event, uvw::PipeHandle& handle)
	{
		m_createdPipes.erase(pipeWeak.lock());
	});

	handle.accept(*pipe);
	pipe->read();

	m_createdPipes.insert(pipe);
}

void UvTcpServer::OnConnection(int status)
{
	// check for error conditions
	if (status < 0)
	{
		trace("error on connection: %s\n", uv_strerror(status));
		return;
	}

	auto clientHandle = m_server->loop().resource<uvw::TCPHandle>();
	m_server->accept(*clientHandle);

	// if not set up yet, don't accept
	if (m_dispatchPipes.empty())
	{
		clientHandle->close();
		return;
	}

	// get a pipe
	auto index = m_dispatchIndex++ % m_dispatchPipes.size();

	if (index == m_dispatchPipes.size())
	{
		// TODO: log this? handle locally?
		return;
	}

#ifdef _WIN32
	if (m_tryDetachFromIOCP)
	{
		auto fd = clientHandle->fileno();

		FILE_COMPLETION_INFORMATION info = { 0 };
		IO_STATUS_BLOCK block;

		if (NtSetInformationFile(fd, &block, &info, sizeof(info), FileReplaceCompletionInformation) == STATUS_INVALID_INFO_CLASS)
		{
			m_tryDetachFromIOCP = false;
		}
	}
#endif

	static char dummyMessage[] = { 1, 2, 3, 4 };
	m_dispatchPipes[index]->write(*clientHandle, dummyMessage, sizeof(dummyMessage));
}

UvTcpChildServer::UvTcpChildServer(UvTcpServer* parent, const std::string& pipeName, const std::array<uint8_t, 16>& pipeMessage, int idx)
	: m_parent(parent), m_pipeName(pipeName), m_pipeMessage(pipeMessage)
{
	m_uvLoopName = fmt::sprintf("tcp%d", idx);

	m_uvLoop = Instance<net::UvLoopManager>::Get()->GetOrCreate(m_uvLoopName);
}

void UvTcpChildServer::Listen()
{
	m_uvLoop->EnqueueCallback([this]()
	{
		auto loop = m_uvLoop->Get();

		auto pipe = loop->resource<uvw::PipeHandle>(true);
		pipe->connect(m_pipeName);

		m_dispatchPipe = pipe;

		pipe->on<uvw::DataEvent>([this](const uvw::DataEvent& ev, uvw::PipeHandle& handle)
		{
			OnConnection(0);
		});

		pipe->on<uvw::ConnectEvent>([this](const uvw::ConnectEvent& ev, uvw::PipeHandle& handle)
		{
			handle.read();

			auto msg = std::unique_ptr<char[]>(new char[m_pipeMessage.size()]);
			memcpy(msg.get(), m_pipeMessage.data(), m_pipeMessage.size());

			handle.write(std::move(msg), m_pipeMessage.size());
		});
	});
}

void UvTcpChildServer::OnConnection(int status)
{
	// initialize a handle for the client
	auto clientHandle = m_dispatchPipe->loop().resource<uvw::TCPHandle>();

	// create a stream instance and associate
	fwRefContainer<UvTcpServerStream> stream(new UvTcpServerStream(this));

	// attempt accepting the connection
	if (stream->Accept(std::move(clientHandle)))
	{
		m_clients.insert(stream);

		// invoke the connection callback
		if (m_parent->GetConnectionCallback())
		{
			m_parent->GetConnectionCallback()(stream);
		}
	}
	else
	{
		stream = nullptr;
	}
}

void UvTcpChildServer::RemoveStream(UvTcpServerStream* stream)
{
	m_clients.erase(stream);
}

UvTcpServerStream::UvTcpServerStream(UvTcpChildServer* server)
	: m_server(server), m_closingClient(false), m_threadId(std::this_thread::get_id())
{

}

UvTcpServerStream::~UvTcpServerStream()
{
	CloseClient();
}

void UvTcpServerStream::CloseClient()
{
	auto client = m_client;

	if (client && !m_closingClient)
	{
		m_closingClient = true;

		decltype(m_writeCallback) writeCallback;

		{
			std::unique_lock<std::shared_mutex> lock(m_writeCallbackMutex);
			writeCallback = std::move(m_writeCallback);
		}

		// before closing (but after eating the write callback!), drain the write list
		HandlePendingWrites();

		if (writeCallback)
		{
			writeCallback->clear();
			writeCallback->close();
		}

		client->clear();

		client->stop();

		client->close();

		m_client = {};
	}
}

bool UvTcpServerStream::Accept(std::shared_ptr<uvw::TCPHandle>&& client)
{
	m_client = std::move(client);

	m_client->noDelay(true);

	// initialize a write callback handle
	{
		std::unique_lock<std::shared_mutex> lock(m_writeCallbackMutex);

		fwRefContainer<UvTcpServerStream> thisRef(this);

		m_writeCallback = m_client->loop().resource<uvw::AsyncHandle>();
		m_writeCallback->on<uvw::AsyncEvent>([thisRef](const uvw::AsyncEvent& event, uvw::AsyncHandle& handle)
		{
			thisRef->HandlePendingWrites();
		});
	}

	m_client->on<uvw::DataEvent>([this](const uvw::DataEvent& event, uvw::TCPHandle& handle)
	{
		HandleRead(event.length, event.data);
	});

	m_client->on<uvw::EndEvent>([this](const uvw::EndEvent& event, uvw::TCPHandle& handle)
	{
		HandleRead(0, nullptr);
	});

	m_client->on<uvw::ErrorEvent>([this](const uvw::ErrorEvent& event, uvw::TCPHandle& handle)
	{
		HandleRead(-1, nullptr);
	});

	// accept and read
	m_server->GetServer()->accept(*m_client);
	m_client->read();

	return true;
}

void UvTcpServerStream::HandleRead(ssize_t nread, const std::unique_ptr<char[]>& buf)
{
	if (nread > 0)
	{
		std::vector<uint8_t> targetBuf(nread);
		memcpy(&targetBuf[0], buf.get(), targetBuf.size());

		if (GetReadCallback())
		{
			GetReadCallback()(targetBuf);
		}
	}
	else if (nread <= 0)
	{
		// hold a reference to ourselves while in this scope
		fwRefContainer<UvTcpServerStream> tempContainer = this;

		Close();
	}
}

PeerAddress UvTcpServerStream::GetPeerAddress()
{
	auto client = m_client;

	if (!client)
	{
		return PeerAddress{};
	}

	sockaddr_storage addr;
	int len = sizeof(addr);

	uv_tcp_getpeername(client->raw(), reinterpret_cast<sockaddr*>(&addr), &len);

	return PeerAddress(reinterpret_cast<sockaddr*>(&addr), static_cast<socklen_t>(len));
}

void UvTcpServerStream::Write(const std::string& data)
{
	auto dataRef = std::unique_ptr<char[]>(new char[data.size()]);
	memcpy(dataRef.get(), data.data(), data.size());

	WriteInternal(std::move(dataRef), data.size());
}

void UvTcpServerStream::Write(const std::vector<uint8_t>& data)
{
	auto dataRef = std::unique_ptr<char[]>(new char[data.size()]);
	memcpy(dataRef.get(), data.data(), data.size());

	WriteInternal(std::move(dataRef), data.size());
}

void UvTcpServerStream::Write(std::string&& data)
{
	auto dataRef = std::unique_ptr<char[]>(new char[data.size()]);
	memcpy(dataRef.get(), data.data(), data.size());

	WriteInternal(std::move(dataRef), data.size());
}

void UvTcpServerStream::Write(std::vector<uint8_t>&& data)
{
	auto dataRef = std::unique_ptr<char[]>(new char[data.size()]);
	memcpy(dataRef.get(), data.data(), data.size());

	WriteInternal(std::move(dataRef), data.size());
}

void UvTcpServerStream::Write(std::unique_ptr<char[]> data, size_t size)
{
	WriteInternal(std::move(data), size);
}

// blindly copypasted from StackOverflow (to allow std::function to store the funcref types with their move semantics)
// TODO: we use this *three times* now, time for a shared header?
template<class F>
struct shared_function
{
	std::shared_ptr<F> f;
	shared_function() = default;
	shared_function(F&& f_) : f(std::make_shared<F>(std::move(f_))) {}
	shared_function(shared_function const&) = default;
	shared_function(shared_function&&) = default;
	shared_function& operator=(shared_function const&) = default;
	shared_function& operator=(shared_function&&) = default;

	template<class...As>
	auto operator()(As&&...as) const
	{
		return (*f)(std::forward<As>(as)...);
	}
};

template<class F>
shared_function<std::decay_t<F>> make_shared_function(F&& f)
{
	return { std::forward<F>(f) };
}

void UvTcpServerStream::WriteInternal(std::unique_ptr<char[]> data, size_t size)
{
	if (!m_client)
	{
		return;
	}

	if (std::this_thread::get_id() == m_threadId)
	{
		auto client = m_client;

		if (client)
		{
			client->write(std::move(data), size);
		}

		return;
	}

	std::shared_lock<std::shared_mutex> lock(m_writeCallbackMutex);

	auto writeCallback = m_writeCallback;

	if (writeCallback)
	{
		// submit the write request
		m_pendingRequests.push(make_shared_function([this, data = std::move(data), size]() mutable
		{
			auto client = m_client;

			if (client)
			{
				client->write(std::move(data), size);
			}
		}));

		// wake the callback
		writeCallback->send();
	}
}

void UvTcpServerStream::ScheduleCallback(TScheduledCallback&& callback)
{
	if (std::this_thread::get_id() == m_threadId)
	{
		callback();
		return;
	}

	std::shared_lock<std::shared_mutex> lock(m_writeCallbackMutex);

	auto writeCallback = m_writeCallback;

	if (writeCallback)
	{
		m_pendingRequests.push(std::move(callback));

		writeCallback->send();
	}
}

void UvTcpServerStream::HandlePendingWrites()
{
	if (!m_client)
	{
		return;
	}

	// as a possible result is closing self, keep reference
	fwRefContainer<UvTcpServerStream> selfRef = this;

	// dequeue pending writes
	TScheduledCallback request;

	while (m_pendingRequests.try_pop(request))
	{
		if (m_client)
		{
			request();
		}
	}
}

void UvTcpServerStream::Close()
{
	// NOTE: potential data race if this gets zeroed/closed right when we try to do something
	if (!m_client)
	{
		return;
	}

	std::shared_ptr<uvw::AsyncHandle> wc;

	{
		std::shared_lock<std::shared_mutex> lock(m_writeCallbackMutex);

		wc = m_writeCallback;
	}

	if (!wc)
	{
		return;
	}

	m_pendingRequests.push([=]()
	{
		// keep a reference in scope
		fwRefContainer<UvTcpServerStream> selfRef = this;

		CloseClient();

		SetReadCallback(TReadCallback());

		// get it locally as we may recurse
		auto closeCallback = GetCloseCallback();

		if (closeCallback)
		{
			SetCloseCallback(TCloseCallback());

			closeCallback();
		}

		m_server->RemoveStream(this);
	});

	// wake the callback
	wc->send();
}
}
