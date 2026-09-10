/*
  spout_bridge.cpp — the ONE translation unit allowed to include Spout.

  Adding `#include "SpoutDX12.h"` (or any other Spout header) to a second file
  re-opens forgejo#4: `SpoutUtils.h` leaks an unnamed namespace of TaskDialog
  scratch variables into global scope through two `using namespace spoututils;`
  directives, and every UI local named `hEdit` or `hCombo` in that TU then
  fails the build under C4459.

  There is a test for this. `private/tools/milk2-probe/test_spout_isolation.py`
  fails if any file other than this one names a Spout header.
*/

#include "spout_bridge.h"

#include "SpoutDX12.h"  // the leak lives here, and stops here

namespace mdrop {

// ─── Sender ──────────────────────────────────────────────────────────────────

struct SpoutSender::Impl {
  spoutDX12 spout;
};

SpoutSender::SpoutSender() : m_impl(std::make_unique<Impl>()) {}
SpoutSender::~SpoutSender() = default;

void SpoutSender::SetName(const char* name) {
  m_impl->spout.SetSenderName(name);
}

bool SpoutSender::Open(ID3D12Device* device, ID3D12CommandQueue* queue) {
  // Spout takes the queue as IUnknown**, which every call site used to cast
  // for itself. Doing it here means the cast exists once.
  IUnknown* q = queue;
  return m_impl->spout.OpenDirectX12(device, queue ? &q : nullptr);
}

bool SpoutSender::WrapResource(ID3D12Resource* resource,
                               ID3D11Resource** wrappedOut,
                               D3D12_RESOURCE_STATES initialState) {
  return m_impl->spout.WrapDX12Resource(resource, wrappedOut, initialState);
}

bool SpoutSender::Send(ID3D11Resource* wrappedResource) {
  return m_impl->spout.SendDX11Resource(wrappedResource);
}

void SpoutSender::Close() {
  m_impl->spout.CloseDirectX12();
}

// ─── Receiver ────────────────────────────────────────────────────────────────

struct SpoutReceiver::Impl {
  spoutDX12 spout;
};

SpoutReceiver::SpoutReceiver() : m_impl(std::make_unique<Impl>()) {}
SpoutReceiver::~SpoutReceiver() = default;

void SpoutReceiver::SetName(const char* name) {
  m_impl->spout.SetReceiverName(name);
}

bool SpoutReceiver::Open(ID3D12Device* device, ID3D12CommandQueue* queue) {
  IUnknown* q = queue;
  return m_impl->spout.OpenDirectX12(device, queue ? &q : nullptr);
}

bool SpoutReceiver::Receive(ID3D12Resource** resource) {
  return m_impl->spout.ReceiveDX12Resource(resource);
}

bool SpoutReceiver::IsUpdated() {
  return m_impl->spout.IsUpdated();
}

unsigned int SpoutReceiver::SenderWidth() {
  return m_impl->spout.GetSenderWidth();
}

unsigned int SpoutReceiver::SenderHeight() {
  return m_impl->spout.GetSenderHeight();
}

void SpoutReceiver::Close() {
  m_impl->spout.CloseDirectX12();
}

std::vector<std::string> SpoutSenderNames() {
  // A throwaway instance is how Spout's own samples enumerate; the list comes
  // from shared memory rather than from anything this object holds.
  spoutDX12 probe;
  return probe.GetSenderList();
}

}  // namespace mdrop
