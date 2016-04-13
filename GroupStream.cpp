#include "GroupStream.h"
#include "ParameterWriter.h"
#include "RTMFP.h"

using namespace std;
using namespace Mona;

GroupStream::GroupStream(UInt16 id) : FlashStream(id), _message3Sent(false), _playing(false), _videoCodecSent(false), _splittedTime(0), _splittedLostRate(0.0), _splittedMediaType(0) {
	DEBUG("GroupStream ", id, " created")
}

GroupStream::~GroupStream() {
	disengage();
	DEBUG("GroupStream ",id," deleted")
}

bool GroupStream::process(PacketReader& packet,FlashWriter& writer, double lostRate) {

	UInt32 time(0);
	GroupStream::ContentType type = (GroupStream::ContentType)packet.read8();

	// if exception, it closes the connection, and print an ERROR message
	switch(type) {

		case GroupStream::GROUP_MEMBER: {
			string member;
			packet.read(PEER_ID_SIZE, member);
			memberHandler(member);
			break;
		}
		case GroupStream::GROUP_INIT: {
			if (packet.read16() != 0x4100) {
				ERROR("Unexpected format for NetGroup ID header")
				break;
			}
			string netGroupId, encryptKey, peerId;
			packet.read(0x40, netGroupId);
			if (packet.read16() != 0x2101) {
				ERROR("Unexpected format for NetGroup ID header")
				break;
			}
			packet.read(0x20, encryptKey);
			if (packet.read32() != 0x2303210F) {
				ERROR("Unexpected format for Peer ID header")
				break;
			}
			packet.read(PEER_ID_SIZE, peerId); 
			OnGroupHandshake::raise(netGroupId, encryptKey, peerId);
			break;
		}
		case GroupStream::GROUP_DATA: {
			string value;
			if (packet.available()) {
				UInt16 size = packet.read16();
				packet.read(size, value);
			}
			INFO("GroupStream ", id, " - NetGroup data message type : ", value)
			break;
		}
		case GroupStream::GROUP_NKNOWN2:
			INFO("GroupStream ", id, " - NetGroup 0E message type")

			// When we receive the 0E NetGroup message type we must send the group message 3
			writer.writeGroupMessage3(_targetID);
			_message3Sent = true;
			break;
		case GroupStream::GROUP_REPORT: {
			INFO("GroupStream ", id, " - NetGroup Report (type 0A)")
			UInt8 size = packet.read8();
			if (size == 1) {
				packet.next();
				size = packet.read8();
			}
			if (size != 8) {
				ERROR("Unexpected 1st parameter size in group message 3")
				break;
			}
			string value, peerId;
			DEBUG("Group message 0A - 1st parameter : ", Util::FormatHex(BIN packet.read(8, value).data(), 8, LOG_BUFFER))
			size = packet.read8();
			DEBUG("Group message 0A - 2nd parameter : ", Util::FormatHex(BIN packet.read(size, value).data(), 8, LOG_BUFFER))
			
			// Loop on each peer of the NetGroup
			while (packet.available() > 4) {
				if (packet.read32() != 0x0022210F) {
					ERROR("Unexpected format for peer infos in the group message 3")
					break;
				}
				packet.read(PEER_ID_SIZE, peerId);
				DEBUG("Group message 0A - Peer ID : ", Util::FormatHex(BIN peerId.data(), PEER_ID_SIZE, LOG_BUFFER))
				DEBUG("Group message 0A - Time elapsed : ", packet.read7BitLongValue())
				size = packet.read8();
				DEBUG("Group message 0A - infos : ", Util::FormatHex(BIN packet.read(size, value).data(), size, LOG_BUFFER))
			}
			if (!_message3Sent) {
				writer.writeGroupMessage3(_targetID);
				_message3Sent = true;
			}
			break;
		}
		case GroupStream::GROUP_INFOS: { // contain the stream name of an eventual publication
			string streamName, data;
			UInt8 sizeName = packet.read8();
			if (sizeName > 1) {
				packet.next(); // 00
				packet.read(sizeName-1, streamName);
				packet.read(packet.available(), data);
				if (OnGroupMedia::raise<false>(streamName, data))
					writer.writeGroupMedia(streamName, data);
			}
			DEBUG("GroupStream ", id, " - Group Media Infos (type 21) : ", streamName)
			break;
		}
		case GroupStream::GROUP_FRAGMENTS_MAP: {
			UInt64 counter = packet.read7BitLongValue();
			DEBUG("GroupStream ", id, " - Group Fragments map (type 22) : ", counter, " ; ", Util::FormatHex(BIN packet.current(), packet.available(), LOG_BUFFER))
			packet.next(packet.available());
			if (!_playing) {
				writer.writeGroupPlay();
				_playing = true;
			}
			break;
		}
		case GroupStream::GROUP_MEDIA_DATA:
			DEBUG("GroupStream ", id, " - Group media message 20 : counter=", packet.read7BitLongValue())
			return FlashStream::process(packet, writer, lostRate); // recursive call, can be audio/video packet or invocation
		case GroupStream::GROUP_MEDIA_START: { // Start a splitted media sequence
			UInt64 counter = packet.read7BitLongValue();
			UInt8 splitNumber = packet.read8(); // counter of the splitted sequence
			_splittedMediaType = packet.read8();
			time = packet.read32();

			DEBUG("GroupStream ", id, " - Group ", (_splittedMediaType == AMF::AUDIO ? "Audio" : (_splittedMediaType == AMF::VIDEO ? "Video" : "Unknown")), " Start Splitted media : counter=", counter, ", time=", time, ", splitNumber=", splitNumber)
			if (_splittedMediaType == AMF::AUDIO || _splittedMediaType == AMF::VIDEO) {
				_splittedTime = time;
				_splittedLostRate = lostRate;
				_splittedContent.clear();
				Buffer::Append(_splittedContent, packet.current(), packet.available());
			}
			else
				ERROR("Media type ", Format<UInt8>("%02X", _splittedMediaType), " not supported (or data decoding error)")
			break;
		}
		case GroupStream::GROUP_MEDIA_NEXT: { // Continue a splitted media sequence
			if (_splittedMediaType != AMF::AUDIO && _splittedMediaType != AMF::VIDEO)
				break;

			UInt64 counter = packet.read7BitLongValue();
			UInt8 splitNumber = packet.read8(); // counter of the splitted sequence
			DEBUG("GroupStream ", id, " - Group next Splitted media : counter=", counter, ", splitNumber=", splitNumber)
			Buffer::Append(_splittedContent, packet.current(), packet.available());
			break;
		}
		case GroupStream::GROUP_MEDIA_END: { // End of a splitted media sequence
			if (_splittedMediaType != AMF::AUDIO && _splittedMediaType != AMF::VIDEO)
				break;

			UInt64 counter = packet.read7BitLongValue();
			Buffer::Append(_splittedContent, packet.current(), packet.available());
			PacketReader content(_splittedContent.data(), _splittedContent.size());

			DEBUG("GroupStream ", id, " - Group ", (_splittedMediaType == AMF::AUDIO ? "Audio" : (_splittedMediaType == AMF::VIDEO ? "Video" : "Unknown")), " End splitted media : counter=", counter)
			if (_splittedMediaType == AMF::AUDIO)
				audioHandler(_splittedTime, content, lostRate);
			else if (_splittedMediaType == AMF::VIDEO)
				videoHandler(_splittedTime, content, lostRate);
			break;
		}

		default:
			ERROR("GroupStream ", id, ", Unpacking type '",Format<UInt8>("%02X",(UInt8)type),"' unknown")
	}

	writer.setCallbackHandle(0);
	return writer.state()!=FlashWriter::CLOSED;
}


void GroupStream::messageHandler(const string& name, AMFReader& message, FlashWriter& writer) {
	/*** Player ***/
	if (name == "closeStream") {
		INFO("Stream ", id, " is closing...")
		// TODO: implement close method
		return;
	}
	
	FlashStream::messageHandler(name, message, writer);
}

void GroupStream::memberHandler(const string& peerId) {

	string id;
	INFO("NetGroup Peer ID added : ", Util::FormatHex(BIN peerId.data(), peerId.size(), id))
	OnNewPeer::raise(_groupId, id);
}
