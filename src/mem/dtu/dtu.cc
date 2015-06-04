/*
 * Copyright (c) 2015, Christian Menard
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the FreeBSD Project.
 */

#include "debug/Dtu.hh"
#include "mem/dtu/dtu.hh"
#include "sim/system.hh"

Dtu::Dtu(DtuParams* p)
  : BaseDtu(p),
    atomicMode(p->system->isAtomicMode()),
    regFile(name() + ".regFile", p->num_endpoints),
    numEndpoints(p->num_endpoints),
    masterId(p->system->getMasterId(name())),
    maxMessageSize(p->max_message_size),
    numCmdEpidBits(p->num_cmd_epid_bits),
    numCmdOffsetBits(p->num_cmd_offset_bits),
    registerAccessLatency(p->register_access_latency),
    commandToSpmRequestLatency(p->command_to_spm_request_latency),
    spmResponseToNocRequestLatency(p->spm_response_to_noc_request_latency),
    nocRequestToSpmRequestLatency(p->noc_request_to_spm_request_latency),
    spmResponseToNocResponseLatency(p->spm_response_to_noc_response_latency),
    executeCommandEvent(*this),
    finishMessageTransmissionEvent(*this),
    incrementWritePtrEvent(*this)
{}

PacketPtr
Dtu::generateRequest(Addr paddr, Addr size, MemCmd cmd)
{
    assert(numCmdEpidBits + numCmdOffsetBits + numCmdOpcodeBits <=
            sizeof(RegFile::reg_t) * 8);

    Request::Flags flags;

    auto req = new Request(paddr, size, flags, masterId);

    auto pkt = new Packet(req, cmd);
    auto pktData = new uint8_t[size];
    pkt->dataDynamic(pktData);

    return pkt;
}

Dtu::Command
Dtu::getCommand()
{
    using reg_t = RegFile::reg_t;

    /*
     *   COMMAND                        0
     * |--------------------------------|
     * |  offset  |   epid   |  opcode  |
     * |--------------------------------|
     */
    reg_t opcodeMask = (1 << numCmdOpcodeBits) - 1;
    reg_t epidMask   = ((1 << numCmdEpidBits) - 1) << numCmdOpcodeBits;
    reg_t offsetMask = ((1 << numCmdOffsetBits) - 1) << (numCmdOpcodeBits + numCmdEpidBits);

    auto reg = regFile.readDtuReg(DtuReg::COMMAND);

    Command cmd;

    cmd.opcode = static_cast<CommandOpcode>(reg & opcodeMask);

    cmd.epId = (reg & epidMask) >> numCmdEpidBits;

    cmd.offset = (reg & offsetMask) >> (numCmdEpidBits + numCmdOpcodeBits);

    return cmd;
}

void
Dtu::executeCommand()
{
    Command cmd = getCommand();

    assert(cmd.epId < numEndpoints);

    switch (cmd.opcode)
    {
    case CommandOpcode::IDLE:
        break;
    case CommandOpcode::START_OPERATION:
        startOperation(cmd);
        break;
    case CommandOpcode::INC_READ_PTR:
        incrementReadPtr(cmd.epId);
        break;
    default:
        // TODO error handling
        panic("Invalid opcode %#x\n", static_cast<RegFile::reg_t>(cmd.opcode));
    }
}

void
Dtu::startOperation(Command& cmd)
{
    EpMode mode = static_cast<EpMode>(regFile.readEpReg(cmd.epId, EpReg::MODE));

    switch (mode)
    {
    case EpMode::RECEIVE_MESSAGE:
        // TODO Error handling
        panic("Ep %u: Cannot start operation on an endpoint"
              "that is configured to receive messages\n", cmd.epId);
        break;
    case EpMode::TRANSMIT_MESSAGE:
        startMessageTransmission(cmd.epId);
        break;
    default:
        // TODO Error handling
        panic("Ep %u: Invalid mode\n", cmd.epId);
    }
}

void
Dtu::startMessageTransmission(unsigned epId)
{
    Addr messageAddr = regFile.readEpReg(epId, EpReg::MESSAGE_ADDR);
    Addr messageSize = regFile.readEpReg(epId, EpReg::MESSAGE_SIZE);

    // TODO error handling
    assert(messageSize > 0);
    assert(messageSize + sizeof(MessageHeader) < maxMessageSize);

    DPRINTF(Dtu, "Endpoint %u starts transmission.\n", epId);
    DPRINTF(Dtu, "Read message of %u Bytes at address %#x from local scratchpad.\n",
                 messageSize,
                 messageAddr);

    // set busy flag
    regFile.setDtuReg(DtuReg::STATUS, 1);

    auto pkt = generateRequest(messageAddr, messageSize, MemCmd::ReadReq);

    auto senderState = new SpmSenderState();
    senderState->epId = epId;
    senderState->isLocalRequest = true;
    senderState->isForwardedRequest = false;

    pkt->pushSenderState(senderState);

    if (atomicMode)
    {
        sendAtomicSpmRequest(pkt);
        completeSpmRequest(pkt);
    }
    else
        schedSpmRequest(pkt, clockEdge(commandToSpmRequestLatency));
}

void
Dtu::finishMessageTransmission()
{
    // reset command register and unset busy flag
    regFile.setDtuReg(DtuReg::COMMAND, 0);
    regFile.setDtuReg(DtuReg::STATUS, 0);
}

void
Dtu::incrementReadPtr(unsigned epId)
{
    Addr readPtr    = regFile.readEpReg(epId, EpReg::BUFFER_READ_PTR);
    Addr bufferAddr = regFile.readEpReg(epId, EpReg::BUFFER_ADDR);
    Addr bufferSize = regFile.readEpReg(epId, EpReg::BUFFER_SIZE);
    Addr messageCount = regFile.readEpReg(epId, EpReg::BUFFER_MESSAGE_COUNT);

    // TODO error handling
    assert(messageCount != 0);

    readPtr += maxMessageSize;

    if (readPtr >= bufferAddr + bufferSize * maxMessageSize)
        readPtr = bufferAddr;

    DPRINTF(Dtu, "Ep %u: Increment the read pointer. New address: %#x\n",
                 epId,
                 readPtr);

    /*
     * XXX Actually an additianally cycle is needed to update the register.
     *     We ignore this delay as it should have no or a very small influence
     *     on the performance of the simulated system.
     */

    regFile.setEpReg(epId, EpReg::BUFFER_READ_PTR, readPtr);
    regFile.setEpReg(epId, EpReg::BUFFER_MESSAGE_COUNT, messageCount - 1);
}

void
Dtu::incrementWritePtr(unsigned epId)
{
    Addr writePtr     = regFile.readEpReg(epId, EpReg::BUFFER_WRITE_PTR);
    Addr bufferAddr   = regFile.readEpReg(epId, EpReg::BUFFER_ADDR);
    Addr bufferSize   = regFile.readEpReg(epId, EpReg::BUFFER_SIZE);
    Addr messageCount = regFile.readEpReg(epId, EpReg::BUFFER_MESSAGE_COUNT);

    assert(messageCount < bufferSize);

    writePtr += maxMessageSize;

    if (writePtr >= bufferAddr + bufferSize * maxMessageSize)
        writePtr = bufferAddr;

    DPRINTF(Dtu, "Ep %u: Increment the write pointer. New address: %#x\n",
                 epId,
                 writePtr);

    regFile.setEpReg(epId, EpReg::BUFFER_WRITE_PTR, writePtr);
    regFile.setEpReg(epId, EpReg::BUFFER_MESSAGE_COUNT, messageCount + 1);
}

void
Dtu::completeNocRequest(PacketPtr pkt)
{
    DPRINTF(Dtu, "Received response from remote DTU -> Transaction finished\n");

    Cycles delay = ticksToCycles(pkt->headerDelay + pkt->payloadDelay);

    // clean up
    delete pkt->req;
    delete pkt;

    schedule(finishMessageTransmissionEvent, clockEdge(delay));
}

void
Dtu::completeSpmRequest(PacketPtr pkt)
{
    assert(!pkt->isError());
    assert(pkt->isResponse());

    DPRINTF(Dtu, "Received response from scratchpad.\n");

    auto senderState = dynamic_cast<SpmSenderState*>(pkt->popSenderState());

    assert(senderState->isLocalRequest || senderState->isForwardedRequest);
    assert(!(senderState->isLocalRequest && senderState->isForwardedRequest));

    if (senderState->isLocalRequest)
        completeLocalSpmRequest(pkt);
    else
        completeForwardedSpmRequest(pkt, senderState->epId);

    delete senderState;
}

void
Dtu::completeLocalSpmRequest(PacketPtr pkt)
{
    assert(pkt->isRead());

    unsigned epid = getCommand().epId;

    unsigned targetCoreId = regFile.readEpReg(epid, EpReg::TARGET_COREID);
    unsigned targetEpId   = regFile.readEpReg(epid, EpReg::TARGET_EPID);
    unsigned messageSize  = regFile.readEpReg(epid, EpReg::MESSAGE_SIZE);

    assert(pkt->getSize() == messageSize);

    DPRINTF(Dtu, "Send message of %u bytes to endpoint %u at core %u.\n",
                 messageSize,
                 targetEpId,
                 targetCoreId);

    MessageHeader header = { static_cast<uint8_t>(coreId),
                             static_cast<uint8_t>(epid),
                             static_cast<uint16_t>(messageSize) };

    auto nocPkt = generateRequest(getNocAddr(targetCoreId, targetEpId),
                                  messageSize + sizeof(MessageHeader),
                                  MemCmd::WriteReq);

    memcpy(nocPkt->getPtr<uint8_t>(), &header, sizeof(MessageHeader));
    memcpy(nocPkt->getPtr<uint8_t>() + sizeof(MessageHeader),
           pkt->getPtr<uint8_t>(),
           messageSize);

    Tick pktHeaderDelay = pkt->headerDelay;
    // XXX is this the right way to go?
    nocPkt->payloadDelay = pkt->payloadDelay;

    // clean up
    delete pkt->req;
    delete pkt;

    auto senderState = new NocSenderState();
    senderState->isMessage = true;
    senderState->isMemoryRequest = false;

    nocPkt->pushSenderState(senderState);

    if (atomicMode)
    {
        sendAtomicNocRequest(nocPkt);
        completeNocRequest(nocPkt);
    }
    else
    {
        Cycles delay = spmResponseToNocRequestLatency;
        delay += ticksToCycles(pktHeaderDelay);
        schedNocRequest(nocPkt, clockEdge(delay));
    }
}

void
Dtu::completeForwardedSpmRequest(PacketPtr pkt, unsigned epId)
{
    assert(pkt->isWrite());

    MessageHeader* header = pkt->getPtr<MessageHeader>();

    if (atomicMode)
    {
        incrementWritePtr(epId);
    }
    else
    {
        DPRINTF(Dtu, "Send response back to EP %u at core %u\n",
                     header->epId,
                     header->coreId);

        Cycles delay = ticksToCycles(pkt->headerDelay + pkt->payloadDelay);
        delay += spmResponseToNocResponseLatency;

        pkt->headerDelay = 0;
        pkt->payloadDelay = 0;

        incrementWritePtrEvent.epId = epId;
        schedule(incrementWritePtrEvent, clockEdge(delay));

        schedNocResponse(pkt, clockEdge(delay));
    }
}

void
Dtu::handleNocRequest(PacketPtr pkt)
{
    assert(!pkt->isError());
    assert(pkt->isWrite());
    assert(pkt->hasData());

    auto senderState = dynamic_cast<NocSenderState*>(pkt->popSenderState());

    assert(senderState->isMessage || senderState->isMemoryRequest);
    assert(!(senderState->isMessage && senderState->isMemoryRequest));

    if (senderState->isMessage)
        recvNocMessage(pkt);
    else
        recvNocMemoryRequest(pkt);

    delete senderState;
}

void
Dtu::recvNocMessage(PacketPtr pkt)
{
    unsigned epId = pkt->getAddr() & ((1UL << nocEpAddrBits) - 1);

    MessageHeader* header = pkt->getPtr<MessageHeader>();

    DPRINTF(Dtu, "EP %u received message of %u bytes from EP %u at core %u\n",
                 epId,
                 header->length,
                 header->epId,
                 header->coreId);

    unsigned messageCount = regFile.readEpReg(epId, EpReg::BUFFER_MESSAGE_COUNT);
    unsigned bufferSize   = regFile.readEpReg(epId, EpReg::BUFFER_SIZE);

    if (messageCount == bufferSize)
        // TODO error handling!
        panic("Ep %u: Buffer full!\n", epId);

    Addr spmAddr = regFile.readEpReg(epId, EpReg::BUFFER_WRITE_PTR);

    DPRINTF(Dtu, "Write message to local scratchpad at address %#x\n", spmAddr);

    pkt->setAddr(spmAddr);

    auto senderState = new SpmSenderState();
    senderState->isLocalRequest = false;
    senderState->isForwardedRequest = true;
    senderState->epId = epId;

    pkt->pushSenderState(senderState);

    if (atomicMode)
    {
        sendAtomicSpmRequest(pkt);
        completeSpmRequest(pkt);
    }
    else
    {
        Cycles delay = ticksToCycles(pkt->headerDelay);
        delay += nocRequestToSpmRequestLatency;

        pkt->headerDelay = 0;

        schedSpmRequest(pkt, clockEdge(delay));
    }
}

void
Dtu::recvNocMemoryRequest(PacketPtr pkt)
{
    panic("recvNocMemoryRequest not yet implemented\n");
}

void
Dtu::handleCpuRequest(PacketPtr pkt)
{
    Addr origAddr = pkt->getAddr();

    // Strip the base address to handle requests based on the register address
    // only. The original address is restored before responding.
    pkt->setAddr(origAddr - cpuBaseAddr);

    bool commandWritten = regFile.handleRequest(pkt);

    pkt->setAddr(origAddr);

    if (!atomicMode)
    {
        /*
         * We handle the request immediatly and do not care about timing. The
         * delay is payed by scheduling the response at some point in the
         * future. Additionaly a write operation on the command register needs
         * to schedule an event that executes this command at a future tick.
         */

        Cycles transportDelay =
            ticksToCycles(pkt->headerDelay + pkt->payloadDelay);

        Tick when = clockEdge(transportDelay + registerAccessLatency);

        pkt->headerDelay = 0;
        pkt->payloadDelay = 0;

        schedCpuResponse(pkt, when);

        if (commandWritten)
            schedule(executeCommandEvent, when);
    }
    else if (commandWritten)
    {
        executeCommand();
    }
}

Dtu*
DtuParams::create()
{
    return new Dtu(this);
}
