/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <Atom/RHI/CommandList.h>
#include <Atom/RHI/DrawListTagRegistry.h>
#include <Atom/RHI/RHISystemInterface.h>
// #include <Atom/Utils/PngFile.h>

#include <Atom/RPI.Public/Pass/CopyPass.h>
#include <Atom/RPI.Public/Pass/PassUtils.h>
#include <Atom/RPI.Public/RPISystemInterface.h>
#include <Atom/RPI.Public/RenderPipeline.h>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/View.h>


namespace AZ
{
    namespace RPI
    {
        // --- Creation & Initialization ---

        CopyPass::~CopyPass()
        {
            ResetInternal();
        }

        Ptr<CopyPass> CopyPass::Create(const PassDescriptor& descriptor)
        {
            Ptr<CopyPass> pass = aznew CopyPass(descriptor);
            return pass;
        }

        CopyPass::CopyPass(const PassDescriptor& descriptor)
            : Pass(descriptor)
        {
            const CopyPassData* copyData = PassUtils::GetPassData<CopyPassData>(descriptor);

            if (copyData)
            {
                m_data = *copyData;

                if (copyData->m_useCopyQueue)
                {
                    m_hardwareQueueClass = RHI::HardwareQueueClass::Copy;
                }
            }
        }

        RHI::CopyItemType CopyPass::GetCopyItemType()
        {
            RHI::AttachmentType inputType = GetInputBinding(0).GetAttachment()->GetAttachmentType();
            RHI::AttachmentType outputType = GetOutputBinding(0).GetAttachment()->GetAttachmentType();

            RHI::CopyItemType copyType = RHI::CopyItemType::Invalid;

            if (inputType == RHI::AttachmentType::Buffer && outputType == RHI::AttachmentType::Buffer)
            {
                copyType = RHI::CopyItemType::Buffer;
            }
            else if (inputType == RHI::AttachmentType::Image && outputType == RHI::AttachmentType::Image)
            {
                copyType = RHI::CopyItemType::Image;
            }
            else if (inputType == RHI::AttachmentType::Buffer && outputType == RHI::AttachmentType::Image)
            {
                copyType = RHI::CopyItemType::BufferToImage;
            }
            else if (inputType == RHI::AttachmentType::Image && outputType == RHI::AttachmentType::Buffer)
            {
                copyType = RHI::CopyItemType::ImageToBuffer;
            }

            return copyType;
        }

        // --- Pass behavior overrides ---

        void CopyPass::BuildInternal()
        {
            AZ_Assert(
                GetInputCount() == 1 && GetOutputCount() == 1,
                "CopyPass has %d inputs and %d outputs. It should have exactly one of each.",
                GetInputCount(),
                GetOutputCount());

            AZ_Assert(
                m_attachmentBindings.size() == 2,
                "CopyPass must have exactly 2 bindings: 1 input and 1 output. %s has %d bindings.",
                GetPathName().GetCStr(),
                m_attachmentBindings.size());

            bool sameDevice = (m_data.m_sourceDeviceIndex == -1 && m_data.m_destinationDeviceIndex == -1) ||
                m_data.m_sourceDeviceIndex == m_data.m_destinationDeviceIndex;
            AZ_Assert(
                sameDevice || (m_data.m_sourceDeviceIndex != -1 && m_data.m_destinationDeviceIndex != -1),
                "CopyPass: Either source and destination device indices must be invalid, or both must be valid");

            m_copyMode = sameDevice ? CopyMode::SameDevice : CopyMode::DifferentDevicesIntermediateHost;

            if (m_copyMode == CopyMode::SameDevice)
            {
                m_copyScopeProducerSameDevice = AZStd::make_shared<RHI::ScopeProducerFunctionNoData>(
                    RHI::ScopeId{ GetPathName() },
                    AZStd::bind(&CopyPass::SetupFrameGraphDependenciesSameDevice, this, AZStd::placeholders::_1),
                    AZStd::bind(&CopyPass::CompileResourcesSameDevice, this, AZStd::placeholders::_1),
                    AZStd::bind(&CopyPass::BuildCommandListInternalSameDevice, this, AZStd::placeholders::_1),
                    m_hardwareQueueClass);
            }

            else if (m_copyMode == CopyMode::DifferentDevicesIntermediateHost)
            {
                auto* device1 = RHI::RHISystemInterface::Get()->GetDevice(RHI::MultiDevice::DefaultDeviceIndex);
                AZ_Assert(
                    device1->GetFeatures().m_signalFenceFromCPU,
                    "CopyPass: Device to device copy is only possible if all devices support signalling fences from the CPU");
                auto* device2 = RHI::RHISystemInterface::Get()->GetDevice(RHI::MultiDevice::DefaultDeviceIndex);
                AZ_Assert(
                    device2->GetFeatures().m_signalFenceFromCPU,
                    "CopyPass: Device to device copy is only possible if all devices support signalling fences from the CPU");

                for (auto& fence : m_device1SignalFence)
                {
                    fence = RHI::Factory::Get().CreateFence();
                    AZ_Assert(fence != nullptr, "CopyPass failed to create a fence");
                    [[maybe_unused]] RHI::ResultCode result = fence->Init(*device1, RHI::FenceState::Signaled);
                    AZ_Assert(result == RHI::ResultCode::Success, "CopyPass failed to init fence");
                }

                for (auto& fence : m_device2WaitFence)
                {
                    fence = RHI::Factory::Get().CreateFence();
                    AZ_Assert(fence != nullptr, "CopyPass failed to create a fence");
                    [[maybe_unused]] auto result = fence->Init(*device2, RHI::FenceState::Signaled);
                    AZ_Assert(result == RHI::ResultCode::Success, "CopyPass failed to init fence");
                }

                m_copyScopeProducerDeviceToHost = AZStd::make_shared<RHI::ScopeProducerFunctionNoData>(
                    RHI::ScopeId{ AZStd::string(GetPathName().GetStringView()) },
                    AZStd::bind(&CopyPass::SetupFrameGraphDependenciesDeviceToHost, this, AZStd::placeholders::_1),
                    AZStd::bind(&CopyPass::CompileResourcesDeviceToHost, this, AZStd::placeholders::_1),
                    AZStd::bind(&CopyPass::BuildCommandListInternalDeviceToHost, this, AZStd::placeholders::_1),
                    m_hardwareQueueClass);

                m_copyScopeProducerHostToDevice = AZStd::make_shared<RHI::ScopeProducerFunctionNoData>(
                    RHI::ScopeId{ AZStd::string(GetPathName().GetStringView()) + "_2" },
                    AZStd::bind(&CopyPass::SetupFrameGraphDependenciesHostToDevice, this, AZStd::placeholders::_1),
                    AZStd::bind(&CopyPass::CompileResourcesHostToDevice, this, AZStd::placeholders::_1),
                    AZStd::bind(&CopyPass::BuildCommandListInternalHostToDevice, this, AZStd::placeholders::_1),
                    m_hardwareQueueClass);
            }
            //@TODO timestamp queries

            // Create transient attachment based on input if required
            if (m_data.m_cloneInput)
            {
                const Ptr<PassAttachment>& source = GetInputBinding(0).GetAttachment();
                Ptr<PassAttachment> dest = source->Clone();

                // Set bind flags to CopyWrite. Other bind flags will be auto-inferred by pass system
                if (dest->m_descriptor.m_type == RHI::AttachmentType::Image)
                {
                    dest->m_descriptor.m_image.m_bindFlags = RHI::ImageBindFlags::CopyWrite;
                }
                else if (dest->m_descriptor.m_type == RHI::AttachmentType::Buffer)
                {
                    dest->m_descriptor.m_buffer.m_bindFlags = RHI::BufferBindFlags::CopyWrite;
                }

                // Set path name for the new attachment and add it to our attachment list
                dest->ComputePathName(GetPathName());
                m_ownedAttachments.push_back(dest);

                // Set the output binding to the new attachment
                GetOutputBinding(0).SetAttachment(dest);
            }
        }

        void CopyPass::FrameBeginInternal(Pass::FramePrepareParams params)
        {
            if (m_copyMode == CopyMode::SameDevice)
            {
                params.m_frameGraphBuilder->ImportScopeProducer(*m_copyScopeProducerSameDevice);
            }
            else if (m_copyMode == CopyMode::DifferentDevicesIntermediateHost)
            {
                params.m_frameGraphBuilder->ImportScopeProducer(*m_copyScopeProducerDeviceToHost);
                params.m_frameGraphBuilder->ImportScopeProducer(*m_copyScopeProducerHostToDevice);
                m_currentBufferIndex = (m_currentBufferIndex + 1) % MaxFrames;
                m_device1SignalFence[m_currentBufferIndex]->Reset();
                m_device2WaitFence[m_currentBufferIndex]->Reset();
            }
        }

        void CopyPass::ResetInternal()
        {
            Pass::ResetInternal();
            if (m_copyMode == CopyMode::DifferentDevicesIntermediateHost)
            {
                for (auto& fence : m_device1SignalFence)
                {
                    fence->WaitOnCpu();
                }
                for (auto& fence : m_device2WaitFence)
                {
                    fence->WaitOnCpu();
                }
            }
        }

        // --- Scope producer functions ---

        void CopyPass::SetupFrameGraphDependenciesSameDevice(RHI::FrameGraphInterface frameGraph)
        {
            for (const PassAttachmentBinding& attachmentBinding : m_attachmentBindings)
            {
                if (attachmentBinding.GetAttachment() != nullptr &&
                    frameGraph.GetAttachmentDatabase().IsAttachmentValid(attachmentBinding.GetAttachment()->GetAttachmentId()))
                {
                    switch (attachmentBinding.m_unifiedScopeDesc.GetType())
                    {
                    case RHI::AttachmentType::Image:
                        {
                            frameGraph.UseAttachment(
                                attachmentBinding.m_unifiedScopeDesc.GetAsImage(),
                                attachmentBinding.GetAttachmentAccess(),
                                attachmentBinding.m_scopeAttachmentUsage);
                            break;
                        }
                    case RHI::AttachmentType::Buffer:
                        {
                            frameGraph.UseAttachment(
                                attachmentBinding.m_unifiedScopeDesc.GetAsBuffer(),
                                attachmentBinding.GetAttachmentAccess(),
                                attachmentBinding.m_scopeAttachmentUsage);
                            break;
                        }
                    default:
                        AZ_Assert(false, "Error, trying to bind an attachment that is neither an image nor a buffer!");
                        break;
                    }
                }
            }
            for (Pass* pass : m_executeAfterPasses)
            {
                RenderPass* renderPass = azrtti_cast<RenderPass*>(pass);
                if (renderPass)
                {
                    frameGraph.ExecuteAfter(renderPass->GetScopeId());
                }
            }
            for (Pass* pass : m_executeBeforePasses)
            {
                RenderPass* renderPass = azrtti_cast<RenderPass*>(pass);
                if (renderPass)
                {
                    frameGraph.ExecuteBefore(renderPass->GetScopeId());
                }
            }
        }

        void CopyPass::CompileResourcesSameDevice(const RHI::FrameGraphCompileContext& context)
        {
            RHI::CopyItemType copyType = GetCopyItemType();
            switch (copyType)
            {
            case AZ::RHI::CopyItemType::Buffer:
                CopyBuffer(context);
                break;
            case AZ::RHI::CopyItemType::Image:
                CopyImage(context);
                break;
            case AZ::RHI::CopyItemType::BufferToImage:
                CopyBufferToImage(context);
                break;
            case AZ::RHI::CopyItemType::ImageToBuffer:
                CopyImageToBuffer(context);
                break;
            default:
                break;
            }
        }

        void CopyPass::BuildCommandListInternalSameDevice(const RHI::FrameGraphExecuteContext& context)
        {
            if (m_copyItemSameDevice.m_type != RHI::CopyItemType::Invalid)
            {
                context.GetCommandList()->Submit(m_copyItemSameDevice.GetDeviceCopyItem(RHI::MultiDevice::DefaultDeviceIndex));
            }
        }

        void CopyPass::SetupFrameGraphDependenciesDeviceToHost(RHI::FrameGraphInterface frameGraph)
        {
            // We need the size of the output image when copying from image to image, so we need all attachments (even the output ones)
            for (const PassAttachmentBinding& attachmentBinding : m_attachmentBindings)
            {
                if (attachmentBinding.GetAttachment() != nullptr &&
                    frameGraph.GetAttachmentDatabase().IsAttachmentValid(attachmentBinding.GetAttachment()->GetAttachmentId()))
                {
                    switch (attachmentBinding.m_unifiedScopeDesc.GetType())
                    {
                    case RHI::AttachmentType::Image:
                        {
                            frameGraph.UseAttachment(
                                attachmentBinding.m_unifiedScopeDesc.GetAsImage(),
                                attachmentBinding.GetAttachmentAccess(),
                                attachmentBinding.m_scopeAttachmentUsage);
                            break;
                        }
                    case RHI::AttachmentType::Buffer:
                        {
                            frameGraph.UseAttachment(
                                attachmentBinding.m_unifiedScopeDesc.GetAsBuffer(),
                                attachmentBinding.GetAttachmentAccess(),
                                attachmentBinding.m_scopeAttachmentUsage);
                            break;
                        }
                    default:
                        AZ_Assert(false, "Error, trying to bind an attachment that is neither an image nor a buffer!");
                        break;
                    }
                }
            }
            for (Pass* pass : m_executeAfterPasses)
            {
                RenderPass* renderPass = azrtti_cast<RenderPass*>(pass);
                if (renderPass)
                {
                    frameGraph.ExecuteAfter(renderPass->GetScopeId());
                }
            }

            frameGraph.SignalFence(*m_device1SignalFence[m_currentBufferIndex]);

            m_device1SignalFence[m_currentBufferIndex]->WaitOnCpuAsync(
                [this, bufferIndex = m_currentBufferIndex]()
                {
                    auto bufferSize = m_device2HostBuffer[bufferIndex]->GetBufferSize();
                    void* data1 = m_device1HostBuffer[bufferIndex]->Map(bufferSize, 0)[RHI::MultiDevice::DefaultDeviceIndex];
                    void* data2 = m_device2HostBuffer[bufferIndex]->Map(bufferSize, 0)[RHI::MultiDevice::DefaultDeviceIndex];
                    memcpy(data2, data1, bufferSize);
                    m_device1HostBuffer[bufferIndex]->Unmap();
                    m_device2HostBuffer[bufferIndex]->Unmap();

                    // m_device1HostBuffer[bufferIndex].reset();

                    m_device2WaitFence[bufferIndex]->SignalOnCpu();
                });
        }

        void CopyPass::CompileResourcesDeviceToHost(const RHI::FrameGraphCompileContext& context)
        {
            RHI::CopyItemType copyType = GetCopyItemType();
            auto inputId = GetInputBinding(0).GetAttachment()->GetAttachmentId();
            switch (copyType)
            {
            case AZ::RHI::CopyItemType::Image:
            case AZ::RHI::CopyItemType::ImageToBuffer:
                {
                    // copy image to read back buffer since only buffer can be accessed by host
                    const AZ::RHI::MultiDeviceImage* sourceImage = context.GetImage(inputId);
                    if (!sourceImage)
                    {
                        AZ_Warning("AttachmentReadback", false, "Failed to find attachment image %s for copy to buffer", inputId.GetCStr());
                        return;
                    }
                    auto sourceImageDescriptor = sourceImage->GetDescriptor();
                    const uint16_t sourceMipSlice = m_data.m_imageSourceSubresource.m_mipSlice;
                    RHI::ImageSubresourceRange sourceRange(sourceMipSlice, sourceMipSlice, 0, 0);
                    sourceRange.m_aspectFlags = RHI::ImageAspectFlags::Color;

                    RHI::ImageAspect sourceImageAspect = RHI::ImageAspect::Color;
                    RHI::ImageAspectFlags sourceImageAspectFlags = RHI::GetImageAspectFlags(sourceImageDescriptor.m_format);
                    if (RHI::CheckBitsAll(sourceImageAspectFlags, RHI::ImageAspectFlags::Depth))
                    {
                        sourceImageAspect = RHI::ImageAspect::Depth;
                        sourceRange.m_aspectFlags = RHI::ImageAspectFlags::Depth;
                    }

                    AZStd::vector<RHI::SingleDeviceImageSubresourceLayout> sourceImageSubResourcesLayouts;
                    sourceImageSubResourcesLayouts.resize_no_construct(sourceImageDescriptor.m_mipLevels);
                    size_t sourceTotalSizeInBytes = 0;
                    sourceImage->GetDeviceImage(RHI::MultiDevice::DefaultDeviceIndex)
                        ->GetSubresourceLayouts(sourceRange, sourceImageSubResourcesLayouts.data(), &sourceTotalSizeInBytes);
                    AZ::u64 sourceByteCount = sourceTotalSizeInBytes;

                    RPI::CommonBufferDescriptor desc;
                    desc.m_poolType = RPI::CommonBufferPoolType::ReadBack;
                    desc.m_bufferName = AZStd::string(GetPathName().GetStringView()) + "_hostbuffer";
                    desc.m_byteCount = sourceByteCount;
                    m_device1HostBuffer[m_currentBufferIndex] = BufferSystemInterface::Get()->CreateBufferFromCommonPool(desc);
                    desc.m_bufferName = AZStd::string(GetPathName().GetStringView()) + "_hostbuffer2";
                    m_device2HostBuffer[m_currentBufferIndex] = BufferSystemInterface::Get()->CreateBufferFromCommonPool(desc);

                    // copy descriptor for copying image to buffer
                    RHI::MultiDeviceCopyImageToBufferDescriptor copyImageToBuffer;
                    copyImageToBuffer.m_mdSourceImage = sourceImage;
                    copyImageToBuffer.m_sourceSize = sourceImageSubResourcesLayouts[sourceMipSlice].m_size;
                    copyImageToBuffer.m_sourceSubresource = RHI::ImageSubresource(sourceMipSlice, 0 /*arraySlice*/, sourceImageAspect);
                    copyImageToBuffer.m_destinationOffset = 0;

                    if (copyType == RHI::CopyItemType::ImageToBuffer)
                    {
                        copyImageToBuffer.m_destinationBytesPerRow = sourceImageSubResourcesLayouts[sourceMipSlice].m_bytesPerRow;
                        copyImageToBuffer.m_destinationBytesPerImage = sourceImageSubResourcesLayouts[sourceMipSlice].m_bytesPerImage;
                        copyImageToBuffer.m_mdDestinationBuffer = m_device1HostBuffer[m_currentBufferIndex]->GetRHIBuffer();
                        copyImageToBuffer.m_destinationFormat = FindFormatForAspect(sourceImageDescriptor.m_format, sourceImageAspect);
                    }
                    else
                    {
                        auto outputId = GetOutputBinding(0).GetAttachment()->GetAttachmentId();
                        const AZ::RHI::MultiDeviceImage* destImage = context.GetImage(outputId);
                        if (!destImage)
                        {
                            AZ_Warning(
                                "AttachmentReadback", false, "Failed to find attachment image %s for copy to buffer", inputId.GetCStr());
                            return;
                        }

                        auto destImageDescriptor = destImage->GetDescriptor();
                        const uint16_t destMipSlice = m_data.m_imageSourceSubresource.m_mipSlice;
                        RHI::ImageSubresourceRange destRange(destMipSlice, destMipSlice, 0, 0);
                        destRange.m_aspectFlags = RHI::ImageAspectFlags::Color;

                        destRange.m_aspectFlags = RHI::ImageAspectFlags::Color;
                        RHI::ImageAspect destImageAspect = RHI::ImageAspect::Color;
                        RHI::ImageAspectFlags destImageAspectFlags = RHI::GetImageAspectFlags(destImageDescriptor.m_format);
                        if (RHI::CheckBitsAll(destImageAspectFlags, RHI::ImageAspectFlags::Depth))
                        {
                            destImageAspect = RHI::ImageAspect::Depth;
                            destRange.m_aspectFlags = RHI::ImageAspectFlags::Depth;
                        }

                        copyImageToBuffer.m_destinationBytesPerRow = sourceImageSubResourcesLayouts[destMipSlice].m_bytesPerRow;
                        copyImageToBuffer.m_destinationBytesPerImage = sourceImageSubResourcesLayouts[destMipSlice].m_bytesPerImage;
                        copyImageToBuffer.m_mdDestinationBuffer = m_device1HostBuffer[m_currentBufferIndex]->GetRHIBuffer();
                        copyImageToBuffer.m_destinationFormat = FindFormatForAspect(destImageDescriptor.m_format, destImageAspect);
                    }

                    m_inputImageLayout = sourceImageSubResourcesLayouts[sourceMipSlice];

                    m_copyItemDeviceToHost = copyImageToBuffer;
                }
                break;
            case AZ::RHI::CopyItemType::Buffer:
            case AZ::RHI::CopyItemType::BufferToImage:
                {
                    const AZ::RHI::MultiDeviceBuffer* buffer = context.GetBuffer(inputId);

                    RPI::CommonBufferDescriptor desc;
                    desc.m_poolType = RPI::CommonBufferPoolType::ReadBack;
                    desc.m_bufferName = AZStd::string(GetPathName().GetStringView()) + "_hostbuffer";
                    desc.m_byteCount = buffer->GetDescriptor().m_byteCount;

                    m_device1HostBuffer[m_currentBufferIndex] = BufferSystemInterface::Get()->CreateBufferFromCommonPool(desc);
                    desc.m_bufferName = AZStd::string(GetPathName().GetStringView()) + "_hostbuffer2";
                    m_device2HostBuffer[m_currentBufferIndex] = BufferSystemInterface::Get()->CreateBufferFromCommonPool(desc);

                    // copy buffer
                    RHI::MultiDeviceCopyBufferDescriptor copyBuffer;
                    copyBuffer.m_mdSourceBuffer = buffer;
                    copyBuffer.m_mdDestinationBuffer = m_device1HostBuffer[m_currentBufferIndex]->GetRHIBuffer();
                    copyBuffer.m_size = aznumeric_cast<uint32_t>(desc.m_byteCount);

                    m_copyItemDeviceToHost = copyBuffer;
                }
                break;
            default:
                break;
            }
        }

        void CopyPass::BuildCommandListInternalDeviceToHost(const RHI::FrameGraphExecuteContext& context)
        {
            if (m_copyItemDeviceToHost.m_type != RHI::CopyItemType::Invalid)
            {
                context.GetCommandList()->Submit(m_copyItemDeviceToHost.GetDeviceCopyItem(RHI::MultiDevice::DefaultDeviceIndex));
            }
        }

        void CopyPass::SetupFrameGraphDependenciesHostToDevice(RHI::FrameGraphInterface frameGraph)
        {
            for (const PassAttachmentBinding& attachmentBinding : m_attachmentBindings)
            {
                // We need a dependency to the input otherwise we get a deadlock
                //    probably because the second scope is inserted before the first one
                {
                    if (attachmentBinding.GetAttachment() != nullptr &&
                        frameGraph.GetAttachmentDatabase().IsAttachmentValid(attachmentBinding.GetAttachment()->GetAttachmentId()))
                    {
                        switch (attachmentBinding.m_unifiedScopeDesc.GetType())
                        {
                        case RHI::AttachmentType::Image:
                            {
                                frameGraph.UseAttachment(
                                    attachmentBinding.m_unifiedScopeDesc.GetAsImage(),
                                    attachmentBinding.GetAttachmentAccess(),
                                    attachmentBinding.m_scopeAttachmentUsage);
                                break;
                            }
                        case RHI::AttachmentType::Buffer:
                            {
                                frameGraph.UseAttachment(
                                    attachmentBinding.m_unifiedScopeDesc.GetAsBuffer(),
                                    attachmentBinding.GetAttachmentAccess(),
                                    attachmentBinding.m_scopeAttachmentUsage);
                                break;
                            }
                        default:
                            AZ_Assert(false, "Error, trying to bind an attachment that is neither an image nor a buffer!");
                            break;
                        }
                    }
                }
            }
            frameGraph.ExecuteAfter(m_copyScopeProducerHostToDevice->GetScopeId());
            for (Pass* pass : m_executeBeforePasses)
            {
                RenderPass* renderPass = azrtti_cast<RenderPass*>(pass);
                if (renderPass)
                {
                    frameGraph.ExecuteBefore(renderPass->GetScopeId());
                }
            }

            frameGraph.WaitFence(*m_device2WaitFence[m_currentBufferIndex]);
        }

        void CopyPass::CompileResourcesHostToDevice(const RHI::FrameGraphCompileContext& context)
        {
            m_copyItemHostToDevice = {};
            m_copyItemHostToDevice.m_type = RHI::CopyItemType::Invalid;
            PassAttachmentBinding& copyDest = GetOutputBinding(0);
            auto outputId = copyDest.GetAttachment()->GetAttachmentId();
            RHI::CopyItemType copyType = GetCopyItemType();
            switch (copyType)
            {
            case AZ::RHI::CopyItemType::Buffer:
            case AZ::RHI::CopyItemType::ImageToBuffer:
                {
                    const AZ::RHI::MultiDeviceBuffer* buffer = context.GetBuffer(outputId);
                    RHI::MultiDeviceCopyBufferDescriptor copyBuffer;
                    copyBuffer.m_mdSourceBuffer = m_device2HostBuffer[m_currentBufferIndex]->GetRHIBuffer();
                    copyBuffer.m_mdDestinationBuffer = buffer;
                    copyBuffer.m_size = aznumeric_cast<uint32_t>(m_device2HostBuffer[m_currentBufferIndex]->GetBufferSize());

                    m_copyItemHostToDevice = copyBuffer;
                }
                break;
            case AZ::RHI::CopyItemType::Image:
            case AZ::RHI::CopyItemType::BufferToImage:
                {
                    RHI::MultiDeviceCopyBufferToImageDescriptor copyDesc;

                    const auto* sourceBuffer = m_device2HostBuffer[m_currentBufferIndex]->GetRHIBuffer();
                    copyDesc.m_mdSourceBuffer = sourceBuffer;

                    copyDesc.m_sourceOffset = 0;
                    if (copyType == RHI::CopyItemType::BufferToImage)
                    {
                        copyDesc.m_sourceBytesPerRow = m_data.m_bufferSourceBytesPerRow;
                        copyDesc.m_sourceBytesPerImage = m_data.m_bufferSourceBytesPerImage;
                        copyDesc.m_sourceSize = m_data.m_sourceSize;
                    }
                    else
                    {
                        copyDesc.m_sourceBytesPerRow = m_inputImageLayout.m_bytesPerRow;
                        copyDesc.m_sourceBytesPerImage = m_inputImageLayout.m_bytesPerImage;
                        copyDesc.m_sourceSize = m_inputImageLayout.m_size;
                    }

                    // Destination Image
                    copyDesc.m_mdDestinationImage = context.GetImage(copyDest.GetAttachment()->GetAttachmentId());
                    copyDesc.m_destinationOrigin = m_data.m_imageDestinationOrigin;
                    copyDesc.m_destinationSubresource = m_data.m_imageDestinationSubresource;

                    m_copyItemHostToDevice = copyDesc;
                }
                break;
            default:
                break;
            }
        }

        void CopyPass::BuildCommandListInternalHostToDevice(const RHI::FrameGraphExecuteContext& context)
        {
            if (m_copyItemHostToDevice.m_type != RHI::CopyItemType::Invalid)
            {
                context.GetCommandList()->Submit(m_copyItemHostToDevice.GetDeviceCopyItem(RHI::MultiDevice::DefaultDeviceIndex));
            }
        }

        // --- Copy setup functions ---

        void CopyPass::CopyBuffer(const RHI::FrameGraphCompileContext& context)
        {
            RHI::MultiDeviceCopyBufferDescriptor copyDesc;
            RHI::MultiDeviceCopyBufferDescriptor copyDesc;

            // Source Buffer
            PassAttachmentBinding& copySource = GetInputBinding(0);
            const auto* sourceBuffer = context.GetBuffer(copySource.GetAttachment()->GetAttachmentId());
            copyDesc.m_mdSourceBuffer = sourceBuffer;
            copyDesc.m_size = static_cast<uint32_t>(sourceBuffer->GetDescriptor().m_byteCount);
            copyDesc.m_sourceOffset = m_data.m_bufferSourceOffset;

            // Destination Buffer
            PassAttachmentBinding& copyDest = GetOutputBinding(0);
            copyDesc.m_mdDestinationBuffer = context.GetBuffer(copyDest.GetAttachment()->GetAttachmentId());
            copyDesc.m_mdDestinationBuffer = context.GetBuffer(copyDest.GetAttachment()->GetAttachmentId());
            copyDesc.m_destinationOffset = m_data.m_bufferDestinationOffset;

            m_copyItemSameDevice = copyDesc;
        }

        void CopyPass::CopyImage(const RHI::FrameGraphCompileContext& context)
        {
            RHI::MultiDeviceCopyImageDescriptor copyDesc;

            // Source Image
            PassAttachmentBinding& copySource = GetInputBinding(0);
            const auto* sourceImage = context.GetImage(copySource.GetAttachment()->GetAttachmentId());
            copyDesc.m_mdSourceImage = sourceImage;
            copyDesc.m_sourceSize = sourceImage->GetDescriptor().m_size;
            copyDesc.m_sourceOrigin = m_data.m_imageSourceOrigin;
            copyDesc.m_sourceSubresource = m_data.m_imageSourceSubresource;

            // Destination Image
            PassAttachmentBinding& copyDest = GetOutputBinding(0);
            copyDesc.m_mdDestinationImage = context.GetImage(copyDest.GetAttachment()->GetAttachmentId());
            copyDesc.m_destinationOrigin = m_data.m_imageDestinationOrigin;
            copyDesc.m_destinationSubresource = m_data.m_imageDestinationSubresource;

            m_copyItemSameDevice = copyDesc;
        }

        void CopyPass::CopyBufferToImage(const RHI::FrameGraphCompileContext& context)
        {
            RHI::MultiDeviceCopyBufferToImageDescriptor copyDesc;

            // Source Buffer
            PassAttachmentBinding& copySource = GetInputBinding(0);
            const auto* sourceBuffer = context.GetBuffer(copySource.GetAttachment()->GetAttachmentId());
            copyDesc.m_mdSourceBuffer = sourceBuffer;
            copyDesc.m_sourceSize = m_data.m_sourceSize;
            copyDesc.m_sourceOffset = m_data.m_bufferSourceOffset;
            copyDesc.m_sourceBytesPerRow = m_data.m_bufferSourceBytesPerRow;
            copyDesc.m_sourceBytesPerImage = m_data.m_bufferSourceBytesPerImage;

            // Destination Image
            PassAttachmentBinding& copyDest = GetOutputBinding(0);
            copyDesc.m_mdDestinationImage = context.GetImage(copyDest.GetAttachment()->GetAttachmentId());
            copyDesc.m_destinationOrigin = m_data.m_imageDestinationOrigin;
            copyDesc.m_destinationSubresource = m_data.m_imageDestinationSubresource;

            m_copyItemSameDevice = copyDesc;
        }

        void CopyPass::CopyImageToBuffer(const RHI::FrameGraphCompileContext& context)
        {
            RHI::MultiDeviceCopyImageToBufferDescriptor copyDesc;

            // Source Image
            PassAttachmentBinding& copySource = GetInputBinding(0);
            const auto* sourceImage = context.GetImage(copySource.GetAttachment()->GetAttachmentId());
            copyDesc.m_mdSourceImage = sourceImage;
            copyDesc.m_sourceSize = sourceImage->GetDescriptor().m_size;
            copyDesc.m_sourceOrigin = m_data.m_imageSourceOrigin;
            copyDesc.m_sourceSubresource = m_data.m_imageSourceSubresource;

            // Destination Buffer
            PassAttachmentBinding& copyDest = GetOutputBinding(0);
            copyDesc.m_mdDestinationBuffer = context.GetBuffer(copyDest.GetAttachment()->GetAttachmentId());
            copyDesc.m_destinationOffset = m_data.m_bufferDestinationOffset;
            copyDesc.m_destinationBytesPerRow = m_data.m_bufferDestinationBytesPerRow;
            copyDesc.m_destinationBytesPerImage = m_data.m_bufferDestinationBytesPerImage;

            m_copyItemSameDevice = copyDesc;
        }

    } // namespace RPI
} // namespace AZ
