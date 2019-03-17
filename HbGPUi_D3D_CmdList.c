#include "HbGPUi_D3D.h"
#if HbGPU_Implementation_D3D

HbBool HbGPU_CmdList_Init(HbGPU_CmdList * cmdList, HbTextU8 const * name, HbGPU_Device * device, HbGPU_CmdQueue queue) {
	cmdList->queue = queue;
	D3D12_COMMAND_LIST_TYPE type;
	switch (queue) {
	case HbGPU_CmdQueue_Graphics:
		type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		break;
	case HbGPU_CmdQueue_Copy:
		type = D3D12_COMMAND_LIST_TYPE_COPY;
		break;
	default:
		return HbFalse;
	}
	if (FAILED(ID3D12Device_CreateCommandAllocator(device->d3dDevice, type,
			&IID_ID3D12CommandAllocator, &cmdList->d3dCommandAllocator))) {
		return HbFalse;
	}
	HbGPUi_D3D_SetSubObjectName(cmdList->d3dCommandAllocator,
			cmdList->d3dCommandAllocator->lpVtbl->SetName, name, "d3dCommandAllocator");
	if (FAILED(ID3D12Device_CreateCommandList(device->d3dDevice, 0, type, cmdList->d3dCommandAllocator,
			HbNull, &IID_ID3D12CommandList, &cmdList->d3dSubmissionCommandList))) {
		ID3D12CommandAllocator_Release(cmdList->d3dCommandAllocator);
		return HbFalse;
	}
	if (FAILED(ID3D12CommandList_QueryInterface(cmdList->d3dSubmissionCommandList,
			&IID_ID3D12GraphicsCommandList, &cmdList->d3dGraphicsCommandList))) {
		ID3D12CommandList_Release(cmdList->d3dSubmissionCommandList);
		ID3D12CommandAllocator_Release(cmdList->d3dCommandAllocator);
		return HbFalse;
	}
	HbGPUi_D3D_SetObjectName(cmdList->d3dGraphicsCommandList, cmdList->d3dGraphicsCommandList->lpVtbl->SetName, name);
	// Command lists are created by D3D in an open state, but it will be opened later.
	ID3D12GraphicsCommandList_Close(cmdList->d3dGraphicsCommandList);
	return HbTrue;
}

void HbGPU_CmdList_Destroy(HbGPU_CmdList * cmdList) {
	ID3D12GraphicsCommandList_Release(cmdList->d3dGraphicsCommandList);
	ID3D12CommandList_Release(cmdList->d3dSubmissionCommandList);
	ID3D12CommandAllocator_Release(cmdList->d3dCommandAllocator);
}

void HbGPU_CmdList_BeginRecording(HbGPU_CmdList * cmdList) {
	ID3D12CommandAllocator_Reset(cmdList->d3dCommandAllocator);
	ID3D12GraphicsCommandList_Reset(cmdList->d3dGraphicsCommandList, cmdList->d3dCommandAllocator, HbNull);
}

void HbGPU_CmdList_Abort(HbGPU_CmdList * cmdList) {
	ID3D12GraphicsCommandList_Close(cmdList->d3dGraphicsCommandList);
}

void HbGPU_CmdList_Submit(HbGPU_Device * device, HbGPU_CmdList * const * cmdLists, uint32_t cmdListCount) {
	if (cmdListCount == 0) {
		return;
	}
	if (cmdListCount == 1) {
		ID3D12GraphicsCommandList_Close(cmdLists[0]->d3dGraphicsCommandList);
		ID3D12CommandQueue * d3dQueue = device->d3dCommandQueues[cmdLists[0]->queue];
		ID3D12CommandQueue_ExecuteCommandLists(d3dQueue, 1, &cmdLists[0]->d3dSubmissionCommandList);
		return;
	}
	// In case there are command lists for different queues, generate arrays of lists to execute on each.
	ID3D12CommandList * * d3dCommandLists = HbStackAlloc(
			HbGPU_CmdQueue_QueueCount * cmdListCount * sizeof(ID3D12CommandList *));
	uint32_t queueCmdListCounts[HbGPU_CmdQueue_QueueCount] = { 0 };
	for (uint32_t cmdListIndex = 0; cmdListIndex < cmdListCount; ++cmdListIndex) {
		HbGPU_CmdList * cmdList = cmdLists[cmdListIndex];
		ID3D12GraphicsCommandList_Close(cmdList->d3dGraphicsCommandList);
		d3dCommandLists[cmdList->queue * cmdListCount + queueCmdListCounts[cmdList->queue]++] =
				cmdList->d3dSubmissionCommandList;
	}
	for (uint32_t queueIndex = 0; queueIndex < HbGPU_CmdQueue_QueueCount; ++queueIndex) {
		if (queueCmdListCounts[queueIndex] != 0) {
			ID3D12CommandQueue * d3dQueue = device->d3dCommandQueues[queueIndex];
			ID3D12CommandQueue_ExecuteCommandLists(d3dQueue, queueCmdListCounts[queueIndex],
					&d3dCommandLists[queueIndex * cmdListCount]);
		}
	}
}

void HbGPU_CmdList_DrawBegin(HbGPU_CmdList * cmdList, HbGPU_DrawPass_Info const * passInfo) {
	cmdList->d3dCurrentDrawPass = *passInfo;
	ID3D12GraphicsCommandList * d3dGraphicsCommandList = cmdList->d3dGraphicsCommandList;
	D3D12_CPU_DESCRIPTOR_HANDLE colorRTDescriptors[HbGPU_RT_MaxBound];
	for (uint32_t rtIndex = 0; rtIndex < passInfo->colorRTCount; ++rtIndex) {
		colorRTDescriptors[rtIndex] = passInfo->colorRTs[rtIndex].d3dHandle;
	}
	ID3D12GraphicsCommandList_OMSetRenderTargets(d3dGraphicsCommandList, passInfo->colorRTCount, colorRTDescriptors, FALSE,
			passInfo->hasDepthStencilRT ? &passInfo->depthStencilRT.d3dHandle : HbNull);
	D3D12_DISCARD_REGION discardRegion = { .NumSubresources = 1 };
	for (uint32_t rtIndex = 0; rtIndex < passInfo->colorRTCount; ++rtIndex) {
		HbGPU_RTReference const * colorRT = &passInfo->colorRTs[rtIndex];
		HbGPU_DrawPass_Actions const * colorActions = &passInfo->colorActions[rtIndex];
		switch (colorActions->beginAction) {
		case HbGPU_DrawPass_BeginAction_Discard:
			discardRegion.FirstSubresource = HbGPUi_D3D_Image_Slice_ToSubresource(&colorRT->d3dImageRef.image->info, colorRT->d3dImageRef.slice);
			ID3D12GraphicsCommandList_DiscardResource(d3dGraphicsCommandList, colorRT->d3dImageRef.image->d3dResource, &discardRegion);
			break;
		case HbGPU_DrawPass_BeginAction_Clear:
			ID3D12GraphicsCommandList_ClearRenderTargetView(d3dGraphicsCommandList, colorRT->d3dHandle, colorActions->clearValue.color, 0, HbNull);
			break;
		default:
			break;
		}
	}
	D3D12_CLEAR_FLAGS depthStencilClearFlags = (D3D12_CLEAR_FLAGS) 0;
	if (passInfo->hasDepthStencilRT) {
		switch (passInfo->depthActions.beginAction) {
		case HbGPU_DrawPass_BeginAction_Discard:
			discardRegion.FirstSubresource = HbGPUi_D3D_Image_Slice_ToSubresource(
					&passInfo->depthStencilRT.d3dImageRef.image->info, passInfo->depthStencilRT.d3dImageRef.slice);
			ID3D12GraphicsCommandList_DiscardResource(d3dGraphicsCommandList, passInfo->depthStencilRT.d3dImageRef.image->d3dResource, &discardRegion);
			break;
		case HbGPU_DrawPass_BeginAction_Clear:
			depthStencilClearFlags |= D3D12_CLEAR_FLAG_DEPTH;
			break;
		default:
			break;
		}
		switch (passInfo->stencilActions.beginAction) {
		case HbGPU_DrawPass_BeginAction_Discard: {
			HbGPU_Image_Slice stencilSlice = passInfo->depthStencilRT.d3dImageRef.slice;
			stencilSlice.stencil = 1;
			discardRegion.FirstSubresource = HbGPUi_D3D_Image_Slice_ToSubresource(&passInfo->depthStencilRT.d3dImageRef.image->info, stencilSlice);
			ID3D12GraphicsCommandList_DiscardResource(d3dGraphicsCommandList, passInfo->depthStencilRT.d3dImageRef.image->d3dResource, &discardRegion);
			break;
		} case HbGPU_DrawPass_BeginAction_Clear:
			depthStencilClearFlags |= D3D12_CLEAR_FLAG_DEPTH;
			break;
		default:
			break;
		}
		if (depthStencilClearFlags != 0) {
			ID3D12GraphicsCommandList_ClearDepthStencilView(d3dGraphicsCommandList, passInfo->depthStencilRT.d3dHandle, depthStencilClearFlags,
					passInfo->depthActions.clearValue.depthStencil.depth, passInfo->stencilActions.clearValue.depthStencil.stencil, 0, HbNull);
		}
	}
}

void HbGPU_CmdList_DrawEnd(HbGPU_CmdList * cmdList) {
	ID3D12GraphicsCommandList * d3dGraphicsCommandList = cmdList->d3dGraphicsCommandList;
	D3D12_DISCARD_REGION discardRegion = { .NumSubresources = 1 };
	for (uint32_t rtIndex = 0; cmdList->d3dCurrentDrawPass.colorRTCount; ++rtIndex) {
		HbGPU_RTReference const * colorRT = &cmdList->d3dCurrentDrawPass.colorRTs[rtIndex];
		HbGPU_DrawPass_Actions const * colorActions = &cmdList->d3dCurrentDrawPass.colorActions[rtIndex];
		if (colorActions->endAction == HbGPU_DrawPass_EndAction_ResolveStore || HbGPU_DrawPass_EndAction_ResolveDiscard) {
			ID3D12GraphicsCommandList_ResolveSubresource(d3dGraphicsCommandList, colorActions->resolveImage->d3dResource,
					HbGPUi_D3D_Image_Slice_ToSubresource(&colorActions->resolveImage->info, colorActions->resolveSlice),
					colorRT->d3dImageRef.image->d3dResource,
					HbGPUi_D3D_Image_Slice_ToSubresource(&colorRT->d3dImageRef.image->info, colorRT->d3dImageRef.slice),
					HbGPUi_D3D_Image_Format_ToTyped(colorActions->resolveImage->info.format));
		}
		if (colorActions->endAction == HbGPU_DrawPass_EndAction_Discard || HbGPU_DrawPass_EndAction_ResolveDiscard) {
			discardRegion.FirstSubresource = HbGPUi_D3D_Image_Slice_ToSubresource(&colorRT->d3dImageRef.image->info, colorRT->d3dImageRef.slice);
			ID3D12GraphicsCommandList_DiscardResource(d3dGraphicsCommandList, colorRT->d3dImageRef.image->d3dResource, &discardRegion);
		}
	}
	if (cmdList->d3dCurrentDrawPass.hasDepthStencilRT) {
		if (cmdList->d3dCurrentDrawPass.depthActions.endAction == HbGPU_DrawPass_EndAction_Discard) {
			discardRegion.FirstSubresource = HbGPUi_D3D_Image_Slice_ToSubresource(
					&cmdList->d3dCurrentDrawPass.depthStencilRT.d3dImageRef.image->info, cmdList->d3dCurrentDrawPass.depthStencilRT.d3dImageRef.slice);
			ID3D12GraphicsCommandList_DiscardResource(d3dGraphicsCommandList,
					cmdList->d3dCurrentDrawPass.depthStencilRT.d3dImageRef.image->d3dResource, &discardRegion);
		}
		if (cmdList->d3dCurrentDrawPass.stencilActions.endAction == HbGPU_DrawPass_EndAction_Discard) {
			HbGPU_Image_Slice stencilSlice = cmdList->d3dCurrentDrawPass.depthStencilRT.d3dImageRef.slice;
			stencilSlice.stencil = 1;
			discardRegion.FirstSubresource = HbGPUi_D3D_Image_Slice_ToSubresource(
					&cmdList->d3dCurrentDrawPass.depthStencilRT.d3dImageRef.image->info, stencilSlice);
			ID3D12GraphicsCommandList_DiscardResource(d3dGraphicsCommandList,
					cmdList->d3dCurrentDrawPass.depthStencilRT.d3dImageRef.image->d3dResource, &discardRegion);
		}
	}
}

#endif
