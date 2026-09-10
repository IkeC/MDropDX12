#pragma once
/*
  spout_bridge.h — the only interface the engine has to Spout.

  WHY THIS EXISTS

  `SpoutUtils.h` opens an **unnamed** namespace inside `namespace spoututils`
  to hold TaskDialog scratch state: `hEdit`, `hCombo`, `bEdit`, `bCombo`,
  `bTopMost`, `hwndTop`, `hTaskIcon`, `stredit`, `comboitems`, `comboindex`.
  `SpoutDirectX.h` and `SpoutGL.h` then both do `using namespace spoututils;`,
  so every one of those names reaches **global scope** in any translation unit
  that includes a Spout header.

  `engine.h` included `SpoutDX12.h`, and nearly everything includes `engine.h`.
  The result was that roughly 100 of the 149 variable-shadowing warnings in the
  2026-08-28 sweep were ordinary UI locals colliding with a texture-sharing
  library's dialog scratch variables. None of them could ever have meant those
  names, so every one was benign — but establishing that cost a full sweep, and
  the collisions came back the moment anyone wrote another dialog with a combo
  box in it.

  Scope tightening does not help: braces cannot un-hide a global, so C4459
  forces a rename every time. And since `d757897` promoted C4456-4459 to level
  3 under `TreatWarningAsError`, a collision is no longer a warning to ignore —
  it fails the build.

  So the vendored headers are included by `spout_bridge.cpp` and nowhere else.
  Everything below is deliberately declared in terms of Direct3D and plain
  types, with the Spout object held behind an opaque `Impl`.

  See forgejo#4. This is containment option 3, which that issue rates best
  because it is the only one that bounds the blast radius to a single file
  rather than relying on nobody re-adding an include.
*/

#ifndef MDROP_SPOUT_BRIDGE_H
#define MDROP_SPOUT_BRIDGE_H

#include <d3d11.h>
#include <d3d12.h>
#include <memory>
#include <string>
#include <vector>

namespace mdrop {

// Publishes a DX12 render target for other applications to receive.
//
// Sending is a two-step dance imposed by Spout: DX12 resources are wrapped as
// DX11 ones via D3D11On12 (`WrapResource`, once per back buffer, after every
// swap-chain recreation), and it is the wrapped DX11 resource that `Send`
// takes.
class SpoutSender {
public:
  SpoutSender();
  ~SpoutSender();
  SpoutSender(const SpoutSender&) = delete;
  SpoutSender& operator=(const SpoutSender&) = delete;

  // ANSI, because that is what Spout's own API takes.
  void SetName(const char* name);

  bool Open(ID3D12Device* device, ID3D12CommandQueue* queue);
  bool WrapResource(ID3D12Resource* resource,
                    ID3D11Resource** wrappedOut,
                    D3D12_RESOURCE_STATES initialState);
  bool Send(ID3D11Resource* wrappedResource);
  void Close();

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

// Receives a texture published by another application's Spout sender.
class SpoutReceiver {
public:
  SpoutReceiver();
  ~SpoutReceiver();
  SpoutReceiver(const SpoutReceiver&) = delete;
  SpoutReceiver& operator=(const SpoutReceiver&) = delete;

  // Empty or unset connects to the first sender available.
  void SetName(const char* name);

  bool Open(ID3D12Device* device, ID3D12CommandQueue* queue);

  // May create or replace the resource, so the caller must compare the
  // returned pointer against what it passed in and re-take ownership when it
  // has changed.
  bool Receive(ID3D12Resource** resource);

  // True when the sender changed size or format, so views must be rebuilt.
  bool IsUpdated();
  unsigned int SenderWidth();
  unsigned int SenderHeight();

  void Close();

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

// Names of the Spout senders currently publishing on this machine.
std::vector<std::string> SpoutSenderNames();

}  // namespace mdrop

#endif  // MDROP_SPOUT_BRIDGE_H
