import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

private let padBridgeDecompressionCallback: VTDecompressionOutputCallback = {
    refCon, _, status, _, imageBuffer, _, _ in
    guard status == noErr, let refCon, let imageBuffer else { return }
    let decoder = Unmanaged<H264Decoder>.fromOpaque(refCon).takeUnretainedValue()
    decoder.deliver(imageBuffer)
}

final class H264Decoder {
    typealias Output = (CVPixelBuffer) -> Void
    var onError: ((String) -> Void)?

    private let output: Output
    private var session: VTDecompressionSession?
    private var formatDescription: CMVideoFormatDescription?
    private var sequenceParameterSet: Data?
    private var pictureParameterSet: Data?

    init(output: @escaping Output) { self.output = output }

    deinit { invalidate() }

    func reset() {
        invalidate()
        sequenceParameterSet = nil
        pictureParameterSet = nil
    }

    func decode(accessUnit: Data, timestampNs: UInt64, keyframe _: Bool) {
        let nals = splitNALUnits(accessUnit)
        var codedNALs: [Data] = []
        for nal in nals where !nal.isEmpty {
            switch nal[0] & 0x1f {
            case 7:
                if sequenceParameterSet != nal {
                    sequenceParameterSet = nal
                    invalidate()
                }
            case 8:
                if pictureParameterSet != nal {
                    pictureParameterSet = nal
                    invalidate()
                }
            case 9:
                break
            default:
                codedNALs.append(nal)
            }
        }
        guard !codedNALs.isEmpty else { return }
        guard ensureSession(), let session, let formatDescription else {
            onError?("Waiting for H.264 SPS/PPS")
            return
        }

        var avcc = Data()
        avcc.reserveCapacity(codedNALs.reduce(0) { $0 + $1.count + 4 })
        for nal in codedNALs {
            avcc.appendBigEndian(UInt32(nal.count))
            avcc.append(nal)
        }

        var blockBuffer: CMBlockBuffer?
        let blockStatus = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault, memoryBlock: nil, blockLength: avcc.count,
            blockAllocator: kCFAllocatorDefault, customBlockSource: nil,
            offsetToData: 0, dataLength: avcc.count, flags: 0,
            blockBufferOut: &blockBuffer)
        guard blockStatus == kCMBlockBufferNoErr, let blockBuffer else {
            onError?("Could not allocate H.264 block buffer")
            return
        }
        let copyStatus = avcc.withUnsafeBytes { bytes in
            CMBlockBufferReplaceDataBytes(with: bytes.baseAddress!, blockBuffer: blockBuffer,
                                          offsetIntoDestination: 0, dataLength: avcc.count)
        }
        guard copyStatus == kCMBlockBufferNoErr else {
            onError?("Could not copy H.264 access unit")
            return
        }

        var timing = CMSampleTimingInfo(
            duration: .invalid,
            presentationTimeStamp: CMTime(value: Int64(timestampNs), timescale: 1_000_000_000),
            decodeTimeStamp: .invalid)
        var sampleSize = avcc.count
        var sampleBuffer: CMSampleBuffer?
        let sampleStatus = CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault, dataBuffer: blockBuffer,
            formatDescription: formatDescription, sampleCount: 1,
            sampleTimingEntryCount: 1, sampleTimingArray: &timing,
            sampleSizeEntryCount: 1, sampleSizeArray: &sampleSize,
            sampleBufferOut: &sampleBuffer)
        guard sampleStatus == noErr, let sampleBuffer else {
            onError?("Could not create H.264 sample")
            return
        }

        let status = VTDecompressionSessionDecodeFrame(
            session, sampleBuffer: sampleBuffer,
            flags: [._EnableAsynchronousDecompression, ._1xRealTimePlayback],
            frameRefcon: nil, infoFlagsOut: nil)
        if status != noErr { onError?("VideoToolbox decode error \(status)") }
    }

    fileprivate func deliver(_ imageBuffer: CVPixelBuffer) { output(imageBuffer) }

    private func ensureSession() -> Bool {
        if session != nil { return true }
        guard let sps = sequenceParameterSet, let pps = pictureParameterSet else { return false }

        var newFormat: CMFormatDescription?
        let formatStatus: OSStatus = sps.withUnsafeBytes { spsBytes in
            pps.withUnsafeBytes { ppsBytes in
                guard let spsPointer = spsBytes.bindMemory(to: UInt8.self).baseAddress,
                      let ppsPointer = ppsBytes.bindMemory(to: UInt8.self).baseAddress else {
                    return kCMFormatDescriptionError_InvalidParameter
                }
                var pointers = [spsPointer, ppsPointer]
                var sizes = [sps.count, pps.count]
                return CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    allocator: kCFAllocatorDefault, parameterSetCount: 2,
                    parameterSetPointers: &pointers, parameterSetSizes: &sizes,
                    nalUnitHeaderLength: 4, formatDescriptionOut: &newFormat)
            }
        }
        guard formatStatus == noErr, let newFormat else {
            onError?("Invalid H.264 parameter sets")
            return false
        }

        let decoderSpecification = [
            kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder as String: true,
            kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder as String: true
        ] as CFDictionary
        let pixelAttributes = [
            kCVPixelBufferPixelFormatTypeKey as String:
                kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferMetalCompatibilityKey as String: true,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:]
        ] as CFDictionary
        var callback = VTDecompressionOutputCallbackRecord(
            decompressionOutputCallback: padBridgeDecompressionCallback,
            decompressionOutputRefCon: Unmanaged.passUnretained(self).toOpaque())
        var newSession: VTDecompressionSession?
        let sessionStatus = VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault, formatDescription: newFormat,
            decoderSpecification: decoderSpecification,
            imageBufferAttributes: pixelAttributes,
            outputCallback: &callback, decompressionSessionOut: &newSession)
        guard sessionStatus == noErr, let newSession else {
            onError?("Hardware H.264 decoder unavailable (\(sessionStatus))")
            return false
        }
        VTSessionSetProperty(newSession, key: kVTDecompressionPropertyKey_RealTime,
                             value: kCFBooleanTrue)
        formatDescription = newFormat
        session = newSession
        return true
    }

    private func invalidate() {
        if let session {
            VTDecompressionSessionWaitForAsynchronousFrames(session)
            VTDecompressionSessionInvalidate(session)
        }
        session = nil
        formatDescription = nil
    }

    private func splitNALUnits(_ data: Data) -> [Data] {
        let bytes = [UInt8](data)
        var starts: [(offset: Int, length: Int)] = []
        var index = 0
        while index + 3 <= bytes.count {
            if bytes[index] == 0 && bytes[index + 1] == 0 && bytes[index + 2] == 1 {
                starts.append((index, 3))
                index += 3
            } else if index + 4 <= bytes.count && bytes[index] == 0 &&
                        bytes[index + 1] == 0 && bytes[index + 2] == 0 && bytes[index + 3] == 1 {
                starts.append((index, 4))
                index += 4
            } else {
                index += 1
            }
        }
        return starts.enumerated().compactMap { item in
            let payloadStart = item.element.offset + item.element.length
            let payloadEnd = item.offset + 1 < starts.count ? starts[item.offset + 1].offset : bytes.count
            guard payloadStart < payloadEnd else { return nil }
            return Data(bytes[payloadStart..<payloadEnd])
        }
    }
}
