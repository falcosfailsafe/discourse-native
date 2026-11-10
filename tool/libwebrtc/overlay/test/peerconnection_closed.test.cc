#include "test/gtest.h"

#include "libwebrtc.h"
#include "rtc_mediaconstraints.h"
#include "rtc_peerconnection.h"
#include "rtc_peerconnection_factory.h"

using libwebrtc::LibWebRTC;
using libwebrtc::scoped_refptr;

// Close() releases the underlying webrtc::PeerConnectionInterface, but the
// Dart side (and any late caller) can still reach the wrapper object during
// teardown. Reading senders()/transceivers()/receivers() or a state used to
// dereference the released pointer and fault with a null read inside
// libwebrtc.dll. Every accessor must now degrade to an empty or "closed"
// answer instead.
TEST(PeerConnectionClosed, AccessorsAreSafeAfterClose) {
  ASSERT_TRUE(LibWebRTC::Initialize());

  scoped_refptr<libwebrtc::RTCPeerConnectionFactory> factory =
      LibWebRTC::CreateRTCPeerConnectionFactory();
  ASSERT_TRUE(factory.get() != nullptr);
  ASSERT_TRUE(factory->Initialize());

  libwebrtc::RTCConfiguration config{};
  scoped_refptr<libwebrtc::RTCMediaConstraints> constraints =
      libwebrtc::RTCMediaConstraints::Create();
  scoped_refptr<libwebrtc::RTCPeerConnection> pc =
      factory->Create(config, constraints);
  ASSERT_TRUE(pc.get() != nullptr);

  // Give the connection something to enumerate while it is open.
  scoped_refptr<libwebrtc::RTCRtpTransceiver> transceiver =
      pc->AddTransceiver(libwebrtc::RTCMediaType::VIDEO);
  ASSERT_TRUE(transceiver.get() != nullptr);
  EXPECT_EQ(pc->transceivers().size(), 1u);

  pc->Close();

  EXPECT_EQ(pc->senders().size(), 0u);
  EXPECT_EQ(pc->transceivers().size(), 0u);
  EXPECT_EQ(pc->receivers().size(), 0u);
  EXPECT_EQ(pc->signaling_state(), libwebrtc::RTCSignalingStateClosed);
  EXPECT_EQ(pc->ice_connection_state(),
            libwebrtc::RTCIceConnectionStateClosed);
  EXPECT_EQ(pc->standardized_ice_connection_state(),
            libwebrtc::RTCIceConnectionStateClosed);
  EXPECT_EQ(pc->peer_connection_state(),
            libwebrtc::RTCPeerConnectionStateClosed);
  EXPECT_TRUE(pc->AddTransceiver(libwebrtc::RTCMediaType::VIDEO).get() ==
              nullptr);
  EXPECT_TRUE(pc->AddTransceiver(libwebrtc::RTCMediaType::AUDIO).get() ==
              nullptr);

  // Closing twice, and deleting a closed connection, must also be safe.
  pc->Close();
  factory->Delete(pc);
  pc = nullptr;
  transceiver = nullptr;
  factory->Terminate();
  factory = nullptr;
  LibWebRTC::Terminate();
}
