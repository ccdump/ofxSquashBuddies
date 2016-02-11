#include "Sender.h"
#include "Constants.h"

namespace ofxSquashBuddies {
	//----------
	Sender::~Sender() {
		this->close();
	}

	//----------
	void Sender::init(string ipAddress, int port) {
		this->close();

		//CHECK IF INIT IS CALLED BEFORE USING THREAD CHANNELS!!!

		//recreate the thread channels
		this->appToCompressor = make_shared<ofThreadChannel<string>>();
		this->compressorToSocket = make_shared<ofThreadChannel<Packet>>();

		this->socket = make_shared<ofxAsio::UDP::Client>();

		{
			auto lock = unique_lock<mutex>(this->configMutex);
			this->config.endPoint = ofxAsio::UDP::EndPoint(ipAddress, port);
		}

		this->threadsRunning = true;
		this->compressThread = thread([this]() {
			this->compressLoop();
		});
		this->socketThread = thread([this]() {
			this->socketLoop();
		});
	}

	//----------
	void Sender::close() {
		this->threadsRunning = false;
		if(this->appToCompressor) {
			this->appToCompressor->close();
			this->appToCompressor.reset();
		}
		if (this->compressorToSocket) {
			this->compressorToSocket->close();
			this->compressorToSocket.reset();
		}

		if (this->compressThread.joinable()) {
			this->compressThread.join();
		}
		if (this->socketThread.joinable()) {
			this->socketThread.join();
		}
	}

	//----------
	void Sender::send(const void * data, size_t size) {
		if (size == 0) {
			OFXSQUASHBUDDIES_WARNING << "Cannot send an empty message";
			return;
		}

		this->appToCompressor->send(string((const char *)data, size));
	}

	//----------
	void Sender::send(const string & data) {
		this->send(data.data(), data.size());
	}

	//----------
	void Sender::send(const ofPixels & pixels) {
		string data;

		//write the message
		{
			const auto headerSize = sizeof(Header::Pixels);
			const auto payloadSize = pixels.size(); // inner payload
			auto messageSize = payloadSize + headerSize;

			data.resize(messageSize);

			auto & header = *static_cast<Header::Pixels *>((void*)data.data());

			header.width = pixels.getWidth();
			header.height = pixels.getHeight();
			header.format = pixels.getPixelFormat();

			memcpy(&data[0] + headerSize, pixels.getData(), payloadSize);
		}

		this->send(data);
	}

	//----------
	void Sender::compressLoop() {
		uint32_t frameIndex = 0;
		while (this->threadsRunning) {
			string message;
			if (this->appToCompressor->receive(message)) {
				Packet packet;

				packet.frameIndex = frameIndex++;

				struct {
					size_t offset = 0;
					size_t availableBytes = Packet::MaxPayloadSize;
				} payloadState;

				ofxSquash::Stream compressStream(this->getCodec(), ofxSquash::Direction::Compress, [this, &packet, &payloadState](const uint8_t * data, size_t size) {
					struct {
						uint8_t * readPosition;
						size_t availableBytes;
					} inputState;
					inputState.readPosition = (uint8_t*)data;
					inputState.availableBytes = size;

					while (inputState.availableBytes > 0) {
						auto bytesToCopy = min<size_t>(size, payloadState.availableBytes);
						memcpy(packet.payload + payloadState.offset, inputState.readPosition, bytesToCopy);

						payloadState.offset += bytesToCopy;
						payloadState.availableBytes -= bytesToCopy;
						inputState.readPosition += bytesToCopy;
						inputState.availableBytes -= bytesToCopy;

						if (payloadState.availableBytes == 0) {
							//finish off the packet header and send whenever we have a full packet
							{
								packet.payloadSize = payloadState.offset;
								this->compressorToSocket->send(packet);
								packet.payloadSize = 0;
							}

							//reset the packet for next use
							{
								payloadState.offset = 0;
								payloadState.availableBytes = Packet::MaxPayloadSize;
							}
						}
					}
				}, Packet::MaxPayloadSize);

				compressStream << message;
				compressStream << ofxSquash::Stream::Finish();

				//if there's anything left over, then send it
				if (payloadState.offset > 0) {
					packet.payloadSize = payloadState.offset;
					this->compressorToSocket->send(packet);
				}
			}
		}
	}

	//----------
	void Sender::socketLoop() {
		while (this->threadsRunning) {
			this->configMutex.lock();
			auto config = this->config;
			this->configMutex.unlock();

			Packet packet;
			if (this->compressorToSocket->receive(packet)) {
				if (this->socket) {
					auto dataGram = make_shared<ofxAsio::UDP::DataGram>();
					dataGram->setEndPoint(config.endPoint);

					dataGram->getMessage().set(&packet, sizeof(packet));
					this->socket->send(dataGram);
				}
			}
		}
	}
}