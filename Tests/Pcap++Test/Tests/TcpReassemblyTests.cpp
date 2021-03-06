#include "../TestDefinition.h"
#include "../Common/TestUtils.h"
#include <sstream>
#include <fstream>
#include <algorithm>
#include "EndianPortable.h"
#include "TcpReassembly.h"
#include "IPv4Layer.h"
#include "TcpLayer.h"
#include "PayloadLayer.h"
#include "PcapFileDevice.h"
#include "PlatformSpecificUtils.h"


// ~~~~~~~~~~~~~~~~~~
// TcpReassemblyStats
// ~~~~~~~~~~~~~~~~~~

struct TcpReassemblyStats
{
	std::string reassembledData;
	int numOfDataPackets;
	int curSide;
	int numOfMessagesFromSide[2];
	bool connectionsStarted;
	bool connectionsEnded;
	bool connectionsEndedManually;
	pcpp::ConnectionData connData;

	TcpReassemblyStats() { clear(); }

	void clear() { reassembledData = ""; numOfDataPackets = 0; curSide = -1; numOfMessagesFromSide[0] = 0; numOfMessagesFromSide[1] = 0; connectionsStarted = false; connectionsEnded = false; connectionsEndedManually = false; }
};


// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// TcpReassemblyMultipleConnStats
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

struct TcpReassemblyMultipleConnStats
{
	typedef std::vector<uint32_t> FlowKeysList;
	typedef std::map<uint32_t, TcpReassemblyStats> Stats;

	Stats stats;
	FlowKeysList flowKeysList;

	void clear()
	{
		stats.clear();
		flowKeysList.clear();
	}

};


// ~~~~~~~~~~~~~~~~~~~~
// readFileIntoString()
// ~~~~~~~~~~~~~~~~~~~~

static std::string readFileIntoString(std::string fileName)
{
	std::ifstream infile(fileName.c_str(), std::ios::binary);
	std::ostringstream ostrm;
	ostrm << infile.rdbuf();
	std::string res = ostrm.str();

	return res;
}


// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// tcpReassemblyMsgReadyCallback()
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

static void tcpReassemblyMsgReadyCallback(int sideIndex, const pcpp::TcpStreamData& tcpData, void* userCookie)
{
	TcpReassemblyMultipleConnStats::Stats &stats = ((TcpReassemblyMultipleConnStats*)userCookie)->stats;

	TcpReassemblyMultipleConnStats::Stats::iterator iter = stats.find(tcpData.getConnectionData().flowKey);
	if (iter == stats.end())
	{
		stats.insert(std::make_pair(tcpData.getConnectionData().flowKey, TcpReassemblyStats()));
		iter = stats.find(tcpData.getConnectionData().flowKey);
	}


	if (sideIndex != iter->second.curSide)
	{
		iter->second.numOfMessagesFromSide[sideIndex]++;
		iter->second.curSide = sideIndex;
	}

	iter->second.numOfDataPackets++;
	iter->second.reassembledData += std::string((char*)tcpData.getData(), tcpData.getDataLength());
}


// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// tcpReassemblyConnectionStartCallback()
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

static void tcpReassemblyConnectionStartCallback(const pcpp::ConnectionData& connectionData, void* userCookie)
{
	TcpReassemblyMultipleConnStats::Stats &stats = ((TcpReassemblyMultipleConnStats*)userCookie)->stats;

	TcpReassemblyMultipleConnStats::Stats::iterator iter = stats.find(connectionData.flowKey);
	if (iter == stats.end())
	{
		stats.insert(std::make_pair(connectionData.flowKey, TcpReassemblyStats()));
		iter = stats.find(connectionData.flowKey);
	}

	TcpReassemblyMultipleConnStats::FlowKeysList &flowKeys = ((TcpReassemblyMultipleConnStats *)userCookie)->flowKeysList;
	if(std::find(flowKeys.begin(), flowKeys.end(), connectionData.flowKey) == flowKeys.end())
		flowKeys.push_back(connectionData.flowKey);

	iter->second.connectionsStarted = true;
	iter->second.connData = connectionData;
}


// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// tcpReassemblyConnectionEndCallback()
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

static void tcpReassemblyConnectionEndCallback(const pcpp::ConnectionData& connectionData, pcpp::TcpReassembly::ConnectionEndReason reason, void* userCookie)
{
	TcpReassemblyMultipleConnStats::Stats &stats = ((TcpReassemblyMultipleConnStats*)userCookie)->stats;

	TcpReassemblyMultipleConnStats::Stats::iterator iter = stats.find(connectionData.flowKey);
	if (iter == stats.end())
	{
		stats.insert(std::make_pair(connectionData.flowKey, TcpReassemblyStats()));
		iter = stats.find(connectionData.flowKey);
	}

	TcpReassemblyMultipleConnStats::FlowKeysList &flowKeys = ((TcpReassemblyMultipleConnStats *)userCookie)->flowKeysList;
	if(std::find(flowKeys.begin(), flowKeys.end(), connectionData.flowKey) == flowKeys.end())
		flowKeys.push_back(connectionData.flowKey);

	if (reason == pcpp::TcpReassembly::TcpReassemblyConnectionClosedManually)
		iter->second.connectionsEndedManually = true;
	else
		iter->second.connectionsEnded = true;
}


// ~~~~~~~~~~~~~~~~~~~
// tcpReassemblyTest()
// ~~~~~~~~~~~~~~~~~~~

static bool tcpReassemblyTest(std::vector<pcpp::RawPacket>& packetStream, TcpReassemblyMultipleConnStats& results, bool monitorOpenCloseConns, bool closeConnsManually)
{
	pcpp::TcpReassembly* tcpReassembly = NULL;

	if (monitorOpenCloseConns)
		tcpReassembly = new pcpp::TcpReassembly(tcpReassemblyMsgReadyCallback, &results, tcpReassemblyConnectionStartCallback, tcpReassemblyConnectionEndCallback);
	else
		tcpReassembly = new pcpp::TcpReassembly(tcpReassemblyMsgReadyCallback, &results);

	for (std::vector<pcpp::RawPacket>::iterator iter = packetStream.begin(); iter != packetStream.end(); iter++)
	{
		pcpp::Packet packet(&(*iter));
		tcpReassembly->reassemblePacket(packet);
	}

	//for(TcpReassemblyMultipleConnStats::Stats::iterator iter = results.stats.begin(); iter != results.stats.end(); iter++)
	//{
	//	// replace \r\n with \n
	//	size_t index = 0;
	//	while (true)
	//	{
	//		 index = iter->second.reassembledData.find("\r\n", index);
	//		 if (index == string::npos) break;
	//		 iter->second.reassembledData.replace(index, 2, "\n");
	//		 index += 1;
	//	}
	//}

	if (closeConnsManually)
		tcpReassembly->closeAllConnections();

	delete tcpReassembly;

	return true;
}


// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// tcpReassemblyAddRetransmissions()
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

static pcpp::RawPacket tcpReassemblyAddRetransmissions(pcpp::RawPacket rawPacket, int beginning, int numOfBytes)
{
	pcpp::Packet packet(&rawPacket);

	pcpp::TcpLayer* tcpLayer = packet.getLayerOfType<pcpp::TcpLayer>();
	if (tcpLayer == NULL)
		throw;

	pcpp::IPv4Layer* ipLayer = packet.getLayerOfType<pcpp::IPv4Layer>();
	if (ipLayer == NULL)
		throw;

	int tcpPayloadSize = be16toh(ipLayer->getIPv4Header()->totalLength)-ipLayer->getHeaderLen()-tcpLayer->getHeaderLen();

	if (numOfBytes <= 0)
		numOfBytes = tcpPayloadSize-beginning;

	uint8_t* newPayload = new uint8_t[numOfBytes];

	if (beginning + numOfBytes <= tcpPayloadSize)
	{
		memcpy(newPayload, tcpLayer->getLayerPayload()+beginning, numOfBytes);
	}
	else
	{
		int bytesToCopy = tcpPayloadSize-beginning;
		memcpy(newPayload, tcpLayer->getLayerPayload()+beginning, bytesToCopy);
		for (int i = bytesToCopy; i < numOfBytes; i++)
		{
			newPayload[i] = '*';
		}
	}

	pcpp::Layer* layerToRemove = tcpLayer->getNextLayer();
	if (layerToRemove != NULL)
		packet.removeLayer(layerToRemove->getProtocol());

	tcpLayer->getTcpHeader()->sequenceNumber = htobe32(be32toh(tcpLayer->getTcpHeader()->sequenceNumber) + beginning);

	pcpp::PayloadLayer newPayloadLayer(newPayload, numOfBytes, false);
	packet.addLayer(&newPayloadLayer);

	packet.computeCalculateFields();

	delete [] newPayload;

	return *(packet.getRawPacket());
}



// ~~~~~~~~~~~~~~~~~~~~~
// ~~~~~~~~~~~~~~~~~~~~~
// Test Cases start here
// ~~~~~~~~~~~~~~~~~~~~~
// ~~~~~~~~~~~~~~~~~~~~~


PTF_TEST_CASE(TestTcpReassemblySanity)
{
	std::string errMsg;
	std::vector<pcpp::RawPacket> packetStream;

	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/one_tcp_stream.pcap", packetStream, errMsg));

	TcpReassemblyMultipleConnStats tcpReassemblyResults;
	tcpReassemblyTest(packetStream, tcpReassemblyResults, true, true);

	TcpReassemblyMultipleConnStats::Stats &stats = tcpReassemblyResults.stats;
	PTF_ASSERT_EQUAL(stats.size(), 1, size);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfDataPackets, 19, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[0], 2, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[1], 2, int);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsStarted);
	PTF_ASSERT_FALSE(stats.begin()->second.connectionsEnded);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsEndedManually);
	PTF_ASSERT_NOT_NULL(stats.begin()->second.connData.srcIP);
	PTF_ASSERT_NOT_NULL(stats.begin()->second.connData.dstIP);
	pcpp::IPv4Address expectedSrcIP(std::string("10.0.0.1"));
	pcpp::IPv4Address expectedDstIP(std::string("81.218.72.15"));
	PTF_ASSERT_TRUE(stats.begin()->second.connData.srcIP->equals(&expectedSrcIP));
	PTF_ASSERT_TRUE(stats.begin()->second.connData.dstIP->equals(&expectedDstIP));
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.startTime.tv_sec, 1491516383, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.startTime.tv_usec, 915793, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.endTime.tv_sec, 0, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.endTime.tv_usec, 0, u64);

	std::string expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_tcp_stream_output.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, stats.begin()->second.reassembledData, string);
} // TestTcpReassemblySanity



PTF_TEST_CASE(TestTcpReassemblyRetran)
{
	std::string errMsg;
	std::vector<pcpp::RawPacket> packetStream;

	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/one_tcp_stream.pcap", packetStream, errMsg));

	// retransmission includes exact same data
	pcpp::RawPacket retPacket1 = tcpReassemblyAddRetransmissions(packetStream.at(4), 0, 0);
	// retransmission includes 10 bytes less than original data (missing bytes are from the beginning)
	pcpp::RawPacket retPacket2 = tcpReassemblyAddRetransmissions(packetStream.at(10), 10, 0);
	// retransmission includes 20 bytes less than original data (missing bytes are from the end)
	pcpp::RawPacket retPacket3 = tcpReassemblyAddRetransmissions(packetStream.at(13), 0, 1340);
	// retransmission includes 10 bytes more than original data (original data + 10 bytes)
	pcpp::RawPacket retPacket4 = tcpReassemblyAddRetransmissions(packetStream.at(21), 0, 1430);
	// retransmission includes 10 bytes less in the beginning and 20 bytes more at the end
	pcpp::RawPacket retPacket5 = tcpReassemblyAddRetransmissions(packetStream.at(28), 10, 1370);
	// retransmission includes 10 bytes less in the beginning and 15 bytes less at the end
	pcpp::RawPacket retPacket6 = tcpReassemblyAddRetransmissions(packetStream.at(34), 10, 91);

	packetStream.insert(packetStream.begin() + 5, retPacket1);
	packetStream.insert(packetStream.begin() + 12, retPacket2);
	packetStream.insert(packetStream.begin() + 16, retPacket3);
	packetStream.insert(packetStream.begin() + 25, retPacket4);
	packetStream.insert(packetStream.begin() + 33, retPacket5);
	packetStream.insert(packetStream.begin() + 40, retPacket6);

	TcpReassemblyMultipleConnStats tcpReassemblyResults;
	tcpReassemblyTest(packetStream, tcpReassemblyResults, false, true);

	TcpReassemblyMultipleConnStats::Stats &stats = tcpReassemblyResults.stats;
	PTF_ASSERT_EQUAL(stats.size(), 1, size);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfDataPackets, 21, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[0], 2, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[1], 2, int);

	std::string expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_tcp_stream_retransmission_output.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, stats.begin()->second.reassembledData, string);
} // TestTcpReassemblyRetran



PTF_TEST_CASE(TestTcpReassemblyMissingData)
{
	std::string errMsg;
	std::vector<pcpp::RawPacket> packetStream;

	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/one_tcp_stream.pcap", packetStream, errMsg));

	// remove 20 bytes from the beginning
	pcpp::RawPacket missPacket1 = tcpReassemblyAddRetransmissions(packetStream.at(3), 20, 0);
	packetStream.insert(packetStream.begin() + 4, missPacket1);
	packetStream.erase(packetStream.begin() + 3);

	// remove 30 bytes from the end
	pcpp::RawPacket missPacket2 = tcpReassemblyAddRetransmissions(packetStream.at(20), 0, 1390);
	packetStream.insert(packetStream.begin() + 21, missPacket2);
	packetStream.erase(packetStream.begin() + 20);

	// remove whole packets
	packetStream.erase(packetStream.begin() + 28);
	packetStream.erase(packetStream.begin() + 30);

	TcpReassemblyMultipleConnStats tcpReassemblyResults;
	tcpReassemblyTest(packetStream, tcpReassemblyResults, false, true);

	TcpReassemblyMultipleConnStats::Stats &stats = tcpReassemblyResults.stats;
	PTF_ASSERT_EQUAL(stats.size(), 1, size);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfDataPackets, 17, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[0], 2, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[1], 2, int);

	std::string expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_tcp_stream_missing_data_output.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, stats.begin()->second.reassembledData, string);

	packetStream.clear();
	tcpReassemblyResults.clear();
	expectedReassemblyData = "";


	// test flow without SYN packet
	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/one_tcp_stream.pcap", packetStream, errMsg));

	// remove SYN and SYN/ACK packets
	packetStream.erase(packetStream.begin());
	packetStream.erase(packetStream.begin());

	tcpReassemblyTest(packetStream, tcpReassemblyResults, false, true);

	PTF_ASSERT_EQUAL(stats.size(), 1, size);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfDataPackets, 19, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[0], 2, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[1], 2, int);

	expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_tcp_stream_output.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, stats.begin()->second.reassembledData, string);
} // TestTcpReassemblyMissingData



PTF_TEST_CASE(TestTcpReassemblyOutOfOrder)
{
	std::string errMsg;
	std::vector<pcpp::RawPacket> packetStream;

	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/one_tcp_stream.pcap", packetStream, errMsg));

	// swap 2 consequent packets
	std::swap(packetStream[9], packetStream[10]);

	// swap 2 non-consequent packets
	pcpp::RawPacket oooPacket1 = packetStream[18];
	packetStream.erase(packetStream.begin() + 18);
	packetStream.insert(packetStream.begin() + 23, oooPacket1);

	// reverse order of all packets in message
	for (int i = 0; i < 12; i++)
	{
		pcpp::RawPacket oooPacketTemp = packetStream[35];
		packetStream.erase(packetStream.begin() + 35);
		packetStream.insert(packetStream.begin() + 24 + i, oooPacketTemp);
	}

	TcpReassemblyMultipleConnStats tcpReassemblyResults;
	tcpReassemblyTest(packetStream, tcpReassemblyResults, true, true);

	TcpReassemblyMultipleConnStats::Stats &stats = tcpReassemblyResults.stats;
	PTF_ASSERT_EQUAL(stats.size(), 1, size);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfDataPackets, 19, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[0], 2, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[1], 2, int);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsStarted);
	PTF_ASSERT_FALSE(stats.begin()->second.connectionsEnded);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsEndedManually);

	std::string expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_tcp_stream_out_of_order_output.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, stats.begin()->second.reassembledData, string);

	packetStream.clear();
	tcpReassemblyResults.clear();
	expectedReassemblyData = "";


	// test out-of-order + missing data
	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/one_tcp_stream.pcap", packetStream, errMsg));

	// reverse order of all packets in message
	for (int i = 0; i < 12; i++)
	{
		pcpp::RawPacket oooPacketTemp = packetStream[35];
		packetStream.erase(packetStream.begin() + 35);
		packetStream.insert(packetStream.begin() + 24 + i, oooPacketTemp);
	}

	// remove one packet
	packetStream.erase(packetStream.begin() + 29);

	tcpReassemblyTest(packetStream, tcpReassemblyResults, true, true);

	PTF_ASSERT_EQUAL(stats.size(), 1, size);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfDataPackets, 18, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[0], 2, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[1], 2, int);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsStarted);
	PTF_ASSERT_FALSE(stats.begin()->second.connectionsEnded);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsEndedManually);

	expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_tcp_stream_missing_data_output_ooo.txt"));

	PTF_ASSERT_EQUAL(expectedReassemblyData, stats.begin()->second.reassembledData, string);
} // TestTcpReassemblyOutOfOrder



PTF_TEST_CASE(TestTcpReassemblyWithFIN_RST)
{
	std::string errMsg;
	std::vector<pcpp::RawPacket> packetStream;
	TcpReassemblyMultipleConnStats tcpReassemblyResults;
	std::string expectedReassemblyData = "";

	// test fin packet in end of connection
	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/one_http_stream_fin.pcap", packetStream, errMsg));
	tcpReassemblyTest(packetStream, tcpReassemblyResults, true, false);

	TcpReassemblyMultipleConnStats::Stats &stats = tcpReassemblyResults.stats;
	PTF_ASSERT_EQUAL(stats.size(), 1, size);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfDataPackets, 5, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[0], 1, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[1], 1, int);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsStarted);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsEnded);
	PTF_ASSERT_FALSE(stats.begin()->second.connectionsEndedManually);
	expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_http_stream_fin_output.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, stats.begin()->second.reassembledData, string);

	packetStream.clear();
	tcpReassemblyResults.clear();
	expectedReassemblyData = "";

	// test rst packet in end of connection
	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/one_http_stream_rst.pcap", packetStream, errMsg));
	tcpReassemblyTest(packetStream, tcpReassemblyResults, true, false);

	PTF_ASSERT_EQUAL(stats.size(), 1, size);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfDataPackets, 2, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[0], 1, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[1], 1, int);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsStarted);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsEnded);
	PTF_ASSERT_FALSE(stats.begin()->second.connectionsEndedManually);
	expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_http_stream_rst_output.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, stats.begin()->second.reassembledData, string);

	packetStream.clear();
	tcpReassemblyResults.clear();
	expectedReassemblyData = "";

	//test fin packet in end of connection that has also data
	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/one_http_stream_fin2.pcap", packetStream, errMsg));
	tcpReassemblyTest(packetStream, tcpReassemblyResults, true, false);

	PTF_ASSERT_EQUAL(stats.size(), 1, size);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfDataPackets, 6, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[0], 1, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[1], 1, int);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsStarted);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsEnded);
	PTF_ASSERT_FALSE(stats.begin()->second.connectionsEndedManually);
	expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_http_stream_fin2_output.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, stats.begin()->second.reassembledData, string);

	packetStream.clear();
	tcpReassemblyResults.clear();
	expectedReassemblyData = "";

	// test missing data before fin
	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/one_http_stream_fin2.pcap", packetStream, errMsg));

	// move second packet of server->client message to the end of the message (after FIN)
	pcpp::RawPacket oooPacketTemp = packetStream[6];
	packetStream.erase(packetStream.begin() + 6);
	packetStream.insert(packetStream.begin() + 12, oooPacketTemp);

	tcpReassemblyTest(packetStream, tcpReassemblyResults, true, false);

	PTF_ASSERT_EQUAL(stats.size(), 1, size);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfDataPackets, 5, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[0], 1, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[1], 1, int);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsStarted);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsEnded);
	PTF_ASSERT_FALSE(stats.begin()->second.connectionsEndedManually);
	expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_http_stream_fin2_output2.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, stats.begin()->second.reassembledData, string);
} // TestTcpReassemblyWithFIN_RST



PTF_TEST_CASE(TestTcpReassemblyMalformedPkts)
{
	std::string errMsg;
	std::vector<pcpp::RawPacket> packetStream;
	TcpReassemblyMultipleConnStats tcpReassemblyResults;
	std::string expectedReassemblyData = "";

	// test retransmission with new data but payload doesn't really contain all the new data
	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/one_http_stream_fin2.pcap", packetStream, errMsg));

	// take one of the packets and increase the IPv4 total length field
	pcpp::Packet malPacket(&packetStream.at(8));
	pcpp::IPv4Layer* ipLayer = malPacket.getLayerOfType<pcpp::IPv4Layer>();
	PTF_ASSERT_NOT_NULL(ipLayer);
	ipLayer->getIPv4Header()->totalLength = be16toh(htobe16(ipLayer->getIPv4Header()->totalLength) + 40);

	tcpReassemblyTest(packetStream, tcpReassemblyResults, true, false);

	TcpReassemblyMultipleConnStats::Stats &stats = tcpReassemblyResults.stats;
	PTF_ASSERT_EQUAL(stats.size(), 1, size);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfDataPackets, 6, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[0], 1, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[1], 1, int);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsStarted);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsEnded);
	PTF_ASSERT_FALSE(stats.begin()->second.connectionsEndedManually);
	expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_http_stream_fin2_output.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, stats.begin()->second.reassembledData, string);
} // TestTcpReassemblyMalformedPkts



PTF_TEST_CASE(TestTcpReassemblyMultipleConns)
{
	TcpReassemblyMultipleConnStats results;
	std::string errMsg;
	std::string expectedReassemblyData = "";

	pcpp::TcpReassembly tcpReassembly(tcpReassemblyMsgReadyCallback, &results, tcpReassemblyConnectionStartCallback, tcpReassemblyConnectionEndCallback);

	std::vector<pcpp::RawPacket> packetStream;
	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/three_http_streams.pcap", packetStream, errMsg));

	pcpp::RawPacket finPacket1 = packetStream.at(13);
	pcpp::RawPacket finPacket2 = packetStream.at(15);

	packetStream.erase(packetStream.begin() + 13);
	packetStream.erase(packetStream.begin() + 14);

	pcpp::TcpReassembly::ReassemblyStatus expectedStatuses[26] = { 
		pcpp::TcpReassembly::TcpMessageHandled, 
		pcpp::TcpReassembly::TcpMessageHandled,
		pcpp::TcpReassembly::TcpMessageHandled, 
		pcpp::TcpReassembly::TcpMessageHandled,
		pcpp::TcpReassembly::Ignore_PacketWithNoData,
		pcpp::TcpReassembly::TcpMessageHandled,
		pcpp::TcpReassembly::TcpMessageHandled,
		pcpp::TcpReassembly::Ignore_PacketWithNoData,
		pcpp::TcpReassembly::TcpMessageHandled,
		pcpp::TcpReassembly::TcpMessageHandled,
		pcpp::TcpReassembly::Ignore_PacketWithNoData,
		pcpp::TcpReassembly::TcpMessageHandled,
		pcpp::TcpReassembly::TcpMessageHandled,
		pcpp::TcpReassembly::Ignore_PacketWithNoData,
		pcpp::TcpReassembly::TcpMessageHandled,
		pcpp::TcpReassembly::FIN_RSTWithNoData,
		pcpp::TcpReassembly::Ignore_PacketWithNoData,
		pcpp::TcpReassembly::FIN_RSTWithNoData,
		pcpp::TcpReassembly::Ignore_PacketWithNoData,
		pcpp::TcpReassembly::Ignore_PacketWithNoData,
		pcpp::TcpReassembly::TcpMessageHandled,
		pcpp::TcpReassembly::Ignore_PacketWithNoData,
		pcpp::TcpReassembly::FIN_RSTWithNoData,
		pcpp::TcpReassembly::FIN_RSTWithNoData,
		pcpp::TcpReassembly::Ignore_PacketWithNoData,
		pcpp::TcpReassembly::Ignore_PacketWithNoData,
	};

	int statusIndex = 0;

	for (std::vector<pcpp::RawPacket>::iterator iter = packetStream.begin(); iter != packetStream.end(); iter++)
	{
		pcpp::Packet packet(&(*iter));
		pcpp::TcpReassembly::ReassemblyStatus status = tcpReassembly.reassemblePacket(packet);
		PTF_ASSERT_EQUAL(status, expectedStatuses[statusIndex++], enum);
	}

	TcpReassemblyMultipleConnStats::Stats &stats = results.stats;
	PTF_ASSERT_EQUAL(stats.size(), 3, size);
	PTF_ASSERT_EQUAL(results.flowKeysList.size(), 3, size);

	TcpReassemblyMultipleConnStats::Stats::iterator iter = stats.begin();

	PTF_ASSERT_EQUAL(iter->second.numOfDataPackets, 2, int);
	PTF_ASSERT_EQUAL(iter->second.numOfMessagesFromSide[0], 1, int);
	PTF_ASSERT_EQUAL(iter->second.numOfMessagesFromSide[1], 1, int);
	PTF_ASSERT_TRUE(iter->second.connectionsStarted);
	PTF_ASSERT_TRUE(iter->second.connectionsEnded);
	PTF_ASSERT_FALSE(iter->second.connectionsEndedManually);
	expectedReassemblyData = readFileIntoString(std::string("PcapExamples/three_http_streams_conn_1_output.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, iter->second.reassembledData, string);

	iter++;

	PTF_ASSERT_EQUAL(iter->second.numOfDataPackets, 2, int);
	PTF_ASSERT_EQUAL(iter->second.numOfMessagesFromSide[0], 1, int);
	PTF_ASSERT_EQUAL(iter->second.numOfMessagesFromSide[1], 1, int);
	PTF_ASSERT_TRUE(iter->second.connectionsStarted);
	PTF_ASSERT_TRUE(iter->second.connectionsEnded);
	PTF_ASSERT_FALSE(iter->second.connectionsEndedManually);
	expectedReassemblyData = readFileIntoString(std::string("PcapExamples/three_http_streams_conn_2_output.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, iter->second.reassembledData, string);

	iter++;

	PTF_ASSERT_EQUAL(iter->second.numOfDataPackets, 2, int);
	PTF_ASSERT_EQUAL(iter->second.numOfMessagesFromSide[0], 1, int);
	PTF_ASSERT_EQUAL(iter->second.numOfMessagesFromSide[1], 1, int);
	PTF_ASSERT_TRUE(iter->second.connectionsStarted);
	PTF_ASSERT_FALSE(iter->second.connectionsEnded);
	PTF_ASSERT_FALSE(iter->second.connectionsEndedManually);
	expectedReassemblyData = readFileIntoString(std::string("PcapExamples/three_http_streams_conn_3_output.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, iter->second.reassembledData, string);


	// test getConnectionInformation and isConnectionOpen

	const pcpp::TcpReassembly::ConnectionInfoList &managedConnections = tcpReassembly.getConnectionInformation();
	PTF_ASSERT_EQUAL(managedConnections.size(), 3, size);

	pcpp::TcpReassembly::ConnectionInfoList::const_iterator iterConn1 = managedConnections.find(results.flowKeysList[0]);
	pcpp::TcpReassembly::ConnectionInfoList::const_iterator iterConn2 = managedConnections.find(results.flowKeysList[1]);
	pcpp::TcpReassembly::ConnectionInfoList::const_iterator iterConn3 = managedConnections.find(results.flowKeysList[2]);
	PTF_ASSERT_NOT_EQUAL(iterConn1, managedConnections.end(), object);
	PTF_ASSERT_NOT_EQUAL(iterConn2, managedConnections.end(), object);
	PTF_ASSERT_NOT_EQUAL(iterConn3, managedConnections.end(), object);
	PTF_ASSERT_GREATER_THAN(tcpReassembly.isConnectionOpen(iterConn1->second), 0, int);
	PTF_ASSERT_EQUAL(tcpReassembly.isConnectionOpen(iterConn2->second), 0, int);
	PTF_ASSERT_EQUAL(tcpReassembly.isConnectionOpen(iterConn3->second), 0, int);

	pcpp::ConnectionData dummyConn;
	dummyConn.flowKey = 0x12345678;
	PTF_ASSERT_LOWER_THAN(tcpReassembly.isConnectionOpen(dummyConn), 0, int);


	// close flow manually and verify it's closed

	tcpReassembly.closeConnection(iter->first);
	PTF_ASSERT_FALSE(iter->second.connectionsEnded);
	PTF_ASSERT_TRUE(iter->second.connectionsEndedManually);


	// now send FIN packets of conn 3 and verify they are ignored

	pcpp::TcpReassembly::ReassemblyStatus status = tcpReassembly.reassemblePacket(&finPacket1);
	PTF_ASSERT_EQUAL(status, pcpp::TcpReassembly::Ignore_PacketOfClosedFlow, enum);
	status = tcpReassembly.reassemblePacket(&finPacket2);
	PTF_ASSERT_EQUAL(status, pcpp::TcpReassembly::Ignore_PacketOfClosedFlow, enum);

	PTF_ASSERT_FALSE(iter->second.connectionsEnded);
	PTF_ASSERT_TRUE(iter->second.connectionsEndedManually);
} // TestTcpReassemblyMultipleConns



PTF_TEST_CASE(TestTcpReassemblyIPv6)
{
	std::string errMsg;
	std::vector<pcpp::RawPacket> packetStream;

	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/one_ipv6_http_stream.pcap", packetStream, errMsg));

	TcpReassemblyMultipleConnStats tcpReassemblyResults;
	tcpReassemblyTest(packetStream, tcpReassemblyResults, true, true);

	TcpReassemblyMultipleConnStats::Stats &stats = tcpReassemblyResults.stats;
	PTF_ASSERT_EQUAL(stats.size(), 1, size);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfDataPackets, 10, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[0], 3, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[1], 3, int);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsStarted);
	PTF_ASSERT_FALSE(stats.begin()->second.connectionsEnded);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsEndedManually);
	PTF_ASSERT_NOT_NULL(stats.begin()->second.connData.srcIP);
	PTF_ASSERT_NOT_NULL(stats.begin()->second.connData.dstIP);
	pcpp::IPv6Address expectedSrcIP(std::string("2001:618:400::5199:cc70"));
	pcpp::IPv6Address expectedDstIP(std::string("2001:618:1:8000::5"));
	PTF_ASSERT_TRUE(stats.begin()->second.connData.srcIP->equals(&expectedSrcIP));
	PTF_ASSERT_TRUE(stats.begin()->second.connData.dstIP->equals(&expectedDstIP));
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.startTime.tv_sec, 1147551796, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.startTime.tv_usec, 702602, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.endTime.tv_sec, 0, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.endTime.tv_usec, 0, u64);

	std::string expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_ipv6_http_stream.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, stats.begin()->second.reassembledData, string);
} // TestTcpReassemblyIPv6



PTF_TEST_CASE(TestTcpReassemblyIPv6MultConns)
{
	std::string errMsg;
	std::vector<pcpp::RawPacket> packetStream;
	std::string expectedReassemblyData = "";

	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/four_ipv6_http_streams.pcap", packetStream, errMsg));

	TcpReassemblyMultipleConnStats tcpReassemblyResults;
	tcpReassemblyTest(packetStream, tcpReassemblyResults, true, true);

	TcpReassemblyMultipleConnStats::Stats &stats = tcpReassemblyResults.stats;
	PTF_ASSERT_EQUAL(stats.size(), 4, size);

	TcpReassemblyMultipleConnStats::Stats::iterator iter = stats.begin();

	pcpp::IPv6Address expectedSrcIP(std::string("2001:618:400::5199:cc70"));
	pcpp::IPv6Address expectedDstIP1(std::string("2001:618:1:8000::5"));
	pcpp::IPv6Address expectedDstIP2(std::string("2001:638:902:1:202:b3ff:feee:5dc2"));

	PTF_ASSERT_EQUAL(iter->second.numOfDataPackets, 14, int);
	PTF_ASSERT_EQUAL(iter->second.numOfMessagesFromSide[0], 3, int);
	PTF_ASSERT_EQUAL(iter->second.numOfMessagesFromSide[1], 3, int);
	PTF_ASSERT_TRUE(iter->second.connectionsStarted);
	PTF_ASSERT_FALSE(iter->second.connectionsEnded);
	PTF_ASSERT_TRUE(iter->second.connectionsEndedManually);
	PTF_ASSERT_NOT_NULL(iter->second.connData.srcIP);
	PTF_ASSERT_NOT_NULL(iter->second.connData.dstIP);
	PTF_ASSERT_TRUE(iter->second.connData.srcIP->equals(&expectedSrcIP));
	PTF_ASSERT_TRUE(iter->second.connData.dstIP->equals(&expectedDstIP1));
	PTF_ASSERT_EQUAL(iter->second.connData.srcPort, 35995, u16);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.startTime.tv_sec, 1147551795, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.startTime.tv_usec, 526632, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.endTime.tv_sec, 0, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.endTime.tv_usec, 0, u64);
	expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_ipv6_http_stream4.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, iter->second.reassembledData, string);

	iter++;

	PTF_ASSERT_EQUAL(iter->second.numOfDataPackets, 10, int);
	PTF_ASSERT_EQUAL(iter->second.numOfMessagesFromSide[0], 1, int);
	PTF_ASSERT_EQUAL(iter->second.numOfMessagesFromSide[1], 1, int);
	PTF_ASSERT_TRUE(iter->second.connectionsStarted);
	PTF_ASSERT_FALSE(iter->second.connectionsEnded);
	PTF_ASSERT_TRUE(iter->second.connectionsEndedManually);
	PTF_ASSERT_NOT_NULL(iter->second.connData.srcIP);
	PTF_ASSERT_NOT_NULL(iter->second.connData.dstIP);
	PTF_ASSERT_TRUE(iter->second.connData.srcIP->equals(&expectedSrcIP));
	PTF_ASSERT_TRUE(iter->second.connData.dstIP->equals(&expectedDstIP1));
	PTF_ASSERT_EQUAL(iter->second.connData.srcPort, 35999, u16);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.startTime.tv_sec, 1147551795, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.startTime.tv_usec, 526632, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.endTime.tv_sec, 0, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.endTime.tv_usec, 0, u64);

	iter++;

	PTF_ASSERT_EQUAL(iter->second.numOfDataPackets, 2, int);
	PTF_ASSERT_EQUAL(iter->second.numOfMessagesFromSide[0], 1, int);
	PTF_ASSERT_EQUAL(iter->second.numOfMessagesFromSide[1], 1, int);
	PTF_ASSERT_TRUE(iter->second.connectionsStarted);
	PTF_ASSERT_FALSE(iter->second.connectionsEnded);
	PTF_ASSERT_TRUE(iter->second.connectionsEndedManually);
	PTF_ASSERT_NOT_NULL(iter->second.connData.srcIP);
	PTF_ASSERT_NOT_NULL(iter->second.connData.dstIP);
	PTF_ASSERT_TRUE(iter->second.connData.srcIP->equals(&expectedSrcIP));
	PTF_ASSERT_TRUE(iter->second.connData.dstIP->equals(&expectedDstIP2));
	PTF_ASSERT_EQUAL(iter->second.connData.srcPort, 40426, u16);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.startTime.tv_sec, 1147551795, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.startTime.tv_usec, 526632, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.endTime.tv_sec, 0, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.endTime.tv_usec, 0, u64);
	expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_ipv6_http_stream3.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, iter->second.reassembledData, string);

	iter++;

	PTF_ASSERT_EQUAL(iter->second.numOfDataPackets, 13, int);
	PTF_ASSERT_EQUAL(iter->second.numOfMessagesFromSide[0], 4, int);
	PTF_ASSERT_EQUAL(iter->second.numOfMessagesFromSide[1], 4, int);
	PTF_ASSERT_TRUE(iter->second.connectionsStarted);
	PTF_ASSERT_FALSE(iter->second.connectionsEnded);
	PTF_ASSERT_TRUE(iter->second.connectionsEndedManually);
	PTF_ASSERT_NOT_NULL(iter->second.connData.srcIP);
	PTF_ASSERT_NOT_NULL(iter->second.connData.dstIP);
	PTF_ASSERT_TRUE(iter->second.connData.srcIP->equals(&expectedSrcIP));
	PTF_ASSERT_TRUE(iter->second.connData.dstIP->equals(&expectedDstIP1));
	PTF_ASSERT_EQUAL(iter->second.connData.srcPort, 35997, u16);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.startTime.tv_sec, 1147551795, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.startTime.tv_usec, 526632, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.endTime.tv_sec, 0, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.endTime.tv_usec, 0, u64);
	expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_ipv6_http_stream2.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, iter->second.reassembledData, string);
} // TestTcpReassemblyIPv6MultConns



PTF_TEST_CASE(TestTcpReassemblyIPv6_OOO)
{
	std::string errMsg;
	std::vector<pcpp::RawPacket> packetStream;

	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/one_ipv6_http_stream.pcap", packetStream, errMsg));

	// swap 2 non-consequent packets
	pcpp::RawPacket oooPacket1 = packetStream[10];
	packetStream.erase(packetStream.begin() + 10);
	packetStream.insert(packetStream.begin() + 12, oooPacket1);

	// swap additional 2 non-consequent packets
	oooPacket1 = packetStream[15];
	packetStream.erase(packetStream.begin() + 15);
	packetStream.insert(packetStream.begin() + 17, oooPacket1);

	TcpReassemblyMultipleConnStats tcpReassemblyResults;
	tcpReassemblyTest(packetStream, tcpReassemblyResults, true, true);

	TcpReassemblyMultipleConnStats::Stats &stats = tcpReassemblyResults.stats;
	PTF_ASSERT_EQUAL(stats.size(), 1, size);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfDataPackets, 10, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[0], 3, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[1], 3, int);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsStarted);
	PTF_ASSERT_FALSE(stats.begin()->second.connectionsEnded);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsEndedManually);
	PTF_ASSERT_NOT_NULL(stats.begin()->second.connData.srcIP);
	PTF_ASSERT_NOT_NULL(stats.begin()->second.connData.dstIP);
	pcpp::IPv6Address expectedSrcIP(std::string("2001:618:400::5199:cc70"));
	pcpp::IPv6Address expectedDstIP(std::string("2001:618:1:8000::5"));
	PTF_ASSERT_TRUE(stats.begin()->second.connData.srcIP->equals(&expectedSrcIP));
	PTF_ASSERT_TRUE(stats.begin()->second.connData.dstIP->equals(&expectedDstIP));
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.startTime.tv_sec, 1147551796, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.startTime.tv_usec, 702602, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.endTime.tv_sec, 0, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.endTime.tv_usec, 0, u64);

	std::string expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_ipv6_http_stream.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, stats.begin()->second.reassembledData, string);
} // TestTcpReassemblyIPv6_OOO



PTF_TEST_CASE(TestTcpReassemblyCleanup)
{
	TcpReassemblyMultipleConnStats results;
	std::string errMsg;

	pcpp::TcpReassemblyConfiguration config(true, 2, 1);
	pcpp::TcpReassembly tcpReassembly(tcpReassemblyMsgReadyCallback, &results, tcpReassemblyConnectionStartCallback, tcpReassemblyConnectionEndCallback, config);

	std::vector<pcpp::RawPacket> packetStream;
	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/three_http_streams.pcap", packetStream, errMsg));

	pcpp::RawPacket lastPacket = packetStream.back();

	packetStream.pop_back();

	for(std::vector<pcpp::RawPacket>::iterator iter = packetStream.begin(); iter != packetStream.end(); iter++)
	{
		pcpp::Packet packet(&(*iter));
		tcpReassembly.reassemblePacket(packet);
	}

	pcpp::TcpReassembly::ConnectionInfoList managedConnections = tcpReassembly.getConnectionInformation(); // make a copy of list
	PTF_ASSERT_EQUAL(managedConnections.size(), 3, size);
	PTF_ASSERT_EQUAL(results.flowKeysList.size(), 3, size);

	pcpp::TcpReassembly::ConnectionInfoList::const_iterator iterConn1 = managedConnections.find(results.flowKeysList[0]);
	pcpp::TcpReassembly::ConnectionInfoList::const_iterator iterConn2 = managedConnections.find(results.flowKeysList[1]);
	pcpp::TcpReassembly::ConnectionInfoList::const_iterator iterConn3 = managedConnections.find(results.flowKeysList[2]);
	PTF_ASSERT_NOT_EQUAL(iterConn1, managedConnections.end(), object);
	PTF_ASSERT_NOT_EQUAL(iterConn2, managedConnections.end(), object);
	PTF_ASSERT_NOT_EQUAL(iterConn3, managedConnections.end(), object);
	PTF_ASSERT_EQUAL(tcpReassembly.isConnectionOpen(iterConn1->second), 0, int);
	PTF_ASSERT_EQUAL(tcpReassembly.isConnectionOpen(iterConn2->second), 0, int);
	PTF_ASSERT_EQUAL(tcpReassembly.isConnectionOpen(iterConn3->second), 0, int);

	PCAP_SLEEP(3);

	tcpReassembly.reassemblePacket(&lastPacket); // automatic cleanup of 1 item
	PTF_ASSERT_EQUAL(tcpReassembly.getConnectionInformation().size(), 2, size);

	tcpReassembly.purgeClosedConnections(); // manually initiated cleanup of 1 item
	PTF_ASSERT_EQUAL(tcpReassembly.getConnectionInformation().size(), 1, size);

	tcpReassembly.purgeClosedConnections(0xFFFFFFFF); // manually initiated cleanup of all items
	PTF_ASSERT_EQUAL(tcpReassembly.getConnectionInformation().size(), 0, size);

	const TcpReassemblyMultipleConnStats::FlowKeysList& flowKeys = results.flowKeysList;
	iterConn1 = managedConnections.find(flowKeys[0]);
	iterConn2 = managedConnections.find(flowKeys[1]);
	iterConn3 = managedConnections.find(flowKeys[2]);
	PTF_ASSERT_NOT_EQUAL(iterConn1, managedConnections.end(), object);
	PTF_ASSERT_NOT_EQUAL(iterConn2, managedConnections.end(), object);
	PTF_ASSERT_NOT_EQUAL(iterConn3, managedConnections.end(), object);
	PTF_ASSERT_EQUAL(tcpReassembly.isConnectionOpen(iterConn1->second), -1, int);
	PTF_ASSERT_EQUAL(tcpReassembly.isConnectionOpen(iterConn2->second), -1, int);
	PTF_ASSERT_EQUAL(tcpReassembly.isConnectionOpen(iterConn3->second), -1, int);
} // TestTcpReassemblyCleanup



PTF_TEST_CASE(TestTcpReassemblyMaxSeq)
{
	std::string errMsg;
	std::vector<pcpp::RawPacket> packetStream;

	PTF_ASSERT_TRUE(readPcapIntoPacketVec("PcapExamples/one_tcp_stream_max_seq.pcap", packetStream, errMsg));

	TcpReassemblyMultipleConnStats tcpReassemblyResults;
	tcpReassemblyTest(packetStream, tcpReassemblyResults, true, true);

	TcpReassemblyMultipleConnStats::Stats &stats = tcpReassemblyResults.stats;
	PTF_ASSERT_EQUAL(stats.size(), 1, size);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfDataPackets, 19, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[0], 2, int);
	PTF_ASSERT_EQUAL(stats.begin()->second.numOfMessagesFromSide[1], 2, int);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsStarted);
	PTF_ASSERT_FALSE(stats.begin()->second.connectionsEnded);
	PTF_ASSERT_TRUE(stats.begin()->second.connectionsEndedManually);
	PTF_ASSERT_NOT_NULL(stats.begin()->second.connData.srcIP);
	PTF_ASSERT_NOT_NULL(stats.begin()->second.connData.dstIP);
	pcpp::IPv4Address expectedSrcIP(std::string("10.0.0.1"));
	pcpp::IPv4Address expectedDstIP(std::string("81.218.72.15"));
	PTF_ASSERT_TRUE(stats.begin()->second.connData.srcIP->equals(&expectedSrcIP));
	PTF_ASSERT_TRUE(stats.begin()->second.connData.dstIP->equals(&expectedDstIP));
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.startTime.tv_sec, 1491516383, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.startTime.tv_usec, 915793, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.endTime.tv_sec, 0, u64);
	PTF_ASSERT_EQUAL(stats.begin()->second.connData.endTime.tv_usec, 0, u64);

	std::string expectedReassemblyData = readFileIntoString(std::string("PcapExamples/one_tcp_stream_output.txt"));
	PTF_ASSERT_EQUAL(expectedReassemblyData, stats.begin()->second.reassembledData, string);
} //TestTcpReassemblyMaxSeq