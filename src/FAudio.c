/* FAudio - XAudio Reimplementation for FNA
 *
 * Copyright (c) 2011-2018 Ethan Lee, Luigi Auriemma, and the MonoGame Team
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from
 * the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 * claim that you wrote the original software. If you use this software in a
 * product, an acknowledgment in the product documentation would be
 * appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not be
 * misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * Ethan "flibitijibibo" Lee <flibitijibibo@flibitijibibo.com>
 *
 */

#include "FAudio_internal.h"

#define MAKE_SUBFORMAT_GUID(guid, fmt) \
	FAudioGUID DATAFORMAT_SUBTYPE_##guid = \
	{ \
		(uint16_t) (fmt), \
		0x0000, \
		0x0010, \
		{ \
			0x80, \
			0x00, \
			0x00, \
			0xAA, \
			0x00, \
			0x38, \
			0x9B, \
			0x71 \
		} \
	}
MAKE_SUBFORMAT_GUID(PCM, 1);
MAKE_SUBFORMAT_GUID(ADPCM, 2);
MAKE_SUBFORMAT_GUID(IEEE_FLOAT, 3);
#undef MAKE_SUBFORMAT_GUID

/* FAudio Interface */

uint32_t FAudioCreate(
	FAudio **ppFAudio,
	uint32_t Flags,
	FAudioProcessor XAudio2Processor
) {
	FAudio_Construct(ppFAudio, FAUDIO_TARGET_VERSION);
	FAudio_Initialize(*ppFAudio, Flags, XAudio2Processor);
	return 0;
}

uint32_t FAudio_Construct(FAudio **ppFAudio, uint8_t version)
{
	FAudio_PlatformAddRef();
	*ppFAudio = (FAudio*) FAudio_malloc(sizeof(FAudio));
	FAudio_zero(*ppFAudio, sizeof(FAudio));
	(*ppFAudio)->version = version;
	(*ppFAudio)->sourceLock = FAudio_PlatformCreateMutex();
	(*ppFAudio)->submixLock = FAudio_PlatformCreateMutex();
	(*ppFAudio)->callbackLock = FAudio_PlatformCreateMutex();
	(*ppFAudio)->refcount = 1;
	return 0;
}

uint32_t FAudio_AddRef(FAudio *audio)
{
	audio->refcount += 1;
	return audio->refcount;
}

uint32_t FAudio_Release(FAudio *audio)
{
	uint32_t refcount;
	audio->refcount -= 1;
	refcount = audio->refcount;
	if (audio->refcount == 0)
	{
		FAudio_StopEngine(audio);
		FAudio_free(audio->decodeCache);
		FAudio_free(audio->resampleCache);
		FAudio_free(audio->effectChainCache);
		FAudio_PlatformDestroyMutex(audio->sourceLock);
		FAudio_PlatformDestroyMutex(audio->submixLock);
		FAudio_PlatformDestroyMutex(audio->callbackLock);
		FAudio_free(audio);
		FAudio_PlatformRelease();
	}
	return refcount;
}

uint32_t FAudio_GetDeviceCount(FAudio *audio, uint32_t *pCount)
{
	*pCount = FAudio_PlatformGetDeviceCount();
	return 0;
}

uint32_t FAudio_GetDeviceDetails(
	FAudio *audio,
	uint32_t Index,
	FAudioDeviceDetails *pDeviceDetails
) {
	FAudio_PlatformGetDeviceDetails(Index, pDeviceDetails);
	return 0;
}

uint32_t FAudio_Initialize(
	FAudio *audio,
	uint32_t Flags,
	FAudioProcessor XAudio2Processor
) {
	FAudio_assert(Flags == 0);
	FAudio_assert(XAudio2Processor == FAUDIO_DEFAULT_PROCESSOR);

	/* FIXME: This is lazy... */
	audio->decodeCache = (float*) FAudio_malloc(sizeof(float));
	audio->resampleCache = (float*) FAudio_malloc(sizeof(float));
	audio->decodeSamples = 1;
	audio->resampleSamples = 1;

	FAudio_StartEngine(audio);
	return 0;
}

uint32_t FAudio_RegisterForCallbacks(
	FAudio *audio,
	FAudioEngineCallback *pCallback
) {
	LinkedList_AddEntry(
		&audio->callbacks,
		pCallback,
		audio->callbackLock
	);
	return 0;
}

void FAudio_UnregisterForCallbacks(
	FAudio *audio,
	FAudioEngineCallback *pCallback
) {
	LinkedList_RemoveEntry(
		&audio->callbacks,
		pCallback,
		audio->callbackLock
	);
}

uint32_t FAudio_CreateSourceVoice(
	FAudio *audio,
	FAudioSourceVoice **ppSourceVoice,
	const FAudioWaveFormatEx *pSourceFormat,
	uint32_t Flags,
	float MaxFrequencyRatio,
	FAudioVoiceCallback *pCallback,
	const FAudioVoiceSends *pSendList,
	const FAudioEffectChain *pEffectChain
) {
	uint32_t i;

	*ppSourceVoice = (FAudioSourceVoice*) FAudio_malloc(sizeof(FAudioVoice));
	FAudio_zero(*ppSourceVoice, sizeof(FAudioSourceVoice));
	(*ppSourceVoice)->audio = audio;
	(*ppSourceVoice)->type = FAUDIO_VOICE_SOURCE;
	(*ppSourceVoice)->flags = Flags;
	(*ppSourceVoice)->filter.Type = FAUDIO_DEFAULT_FILTER_TYPE;
	(*ppSourceVoice)->filter.Frequency = FAUDIO_DEFAULT_FILTER_FREQUENCY;
	(*ppSourceVoice)->filter.OneOverQ = FAUDIO_DEFAULT_FILTER_ONEOVERQ;
	(*ppSourceVoice)->sendLock = FAudio_PlatformCreateMutex();
	(*ppSourceVoice)->effectLock = FAudio_PlatformCreateMutex();
	(*ppSourceVoice)->filterLock = FAudio_PlatformCreateMutex();
	(*ppSourceVoice)->volumeLock = FAudio_PlatformCreateMutex();

	/* Source Properties */
	FAudio_assert(MaxFrequencyRatio <= FAUDIO_MAX_FREQ_RATIO);
	(*ppSourceVoice)->src.maxFreqRatio = MaxFrequencyRatio;

	if (	pSourceFormat->wFormatTag == FAUDIO_FORMAT_PCM ||
		pSourceFormat->wFormatTag == FAUDIO_FORMAT_IEEE_FLOAT	)
	{
		FAudioWaveFormatExtensible *fmtex = (FAudioWaveFormatExtensible*) FAudio_malloc(
			sizeof(FAudioWaveFormatExtensible)
		);
		/* convert PCM to EXTENSIBLE */
		fmtex->Format.wFormatTag = FAUDIO_FORMAT_EXTENSIBLE;
		fmtex->Format.nChannels = pSourceFormat->nChannels;
		fmtex->Format.nSamplesPerSec = pSourceFormat->nSamplesPerSec;
		fmtex->Format.nAvgBytesPerSec = pSourceFormat->nAvgBytesPerSec;
		fmtex->Format.nBlockAlign = pSourceFormat->nBlockAlign;
		fmtex->Format.wBitsPerSample = pSourceFormat->wBitsPerSample;
		fmtex->Format.cbSize = sizeof(FAudioWaveFormatExtensible) - sizeof(FAudioWaveFormatEx);
		fmtex->Samples.wValidBitsPerSample = pSourceFormat->wBitsPerSample;
		fmtex->dwChannelMask = 0;
		if (pSourceFormat->wFormatTag == FAUDIO_FORMAT_PCM)
		{
			FAudio_memcpy(&fmtex->SubFormat, &DATAFORMAT_SUBTYPE_PCM, sizeof(FAudioGUID));
		}
		else if (pSourceFormat->wFormatTag == FAUDIO_FORMAT_IEEE_FLOAT)
		{
			FAudio_memcpy(&fmtex->SubFormat, &DATAFORMAT_SUBTYPE_IEEE_FLOAT, sizeof(FAudioGUID));
		}
		(*ppSourceVoice)->src.format = &fmtex->Format;
	}
	else
	{
		/* direct copy anything else */
		(*ppSourceVoice)->src.format = (FAudioWaveFormatEx*) FAudio_malloc(
			sizeof(FAudioWaveFormatEx) + pSourceFormat->cbSize
		);
		FAudio_memcpy(
			(*ppSourceVoice)->src.format,
			pSourceFormat,
			sizeof(FAudioWaveFormatEx) + pSourceFormat->cbSize
		);
	}

	(*ppSourceVoice)->src.callback = pCallback;
	(*ppSourceVoice)->src.active = 0;
	(*ppSourceVoice)->src.freqRatio = 1.0f;
	(*ppSourceVoice)->src.totalSamples = 0;
	(*ppSourceVoice)->src.bufferList = NULL;
	(*ppSourceVoice)->src.bufferLock = FAudio_PlatformCreateMutex();

	if ((*ppSourceVoice)->src.format->wFormatTag == FAUDIO_FORMAT_EXTENSIBLE)
	{
		FAudioWaveFormatExtensible *fmtex = (FAudioWaveFormatExtensible*)(*ppSourceVoice)->src.format;

		if (FAudio_memcmp(&fmtex->SubFormat, &DATAFORMAT_SUBTYPE_PCM, sizeof(FAudioGUID)) == 0)
		{
			(*ppSourceVoice)->src.decode = (fmtex->Format.wBitsPerSample == 16) ?
				FAudio_INTERNAL_DecodePCM16 :
				FAudio_INTERNAL_DecodePCM8;
		}
		else if (FAudio_memcmp(&fmtex->SubFormat, &DATAFORMAT_SUBTYPE_IEEE_FLOAT, sizeof(FAudioGUID)) == 0)
		{
			(*ppSourceVoice)->src.decode = FAudio_INTERNAL_DecodePCM32F;
		}
		else
		{
			FAudio_assert(0 && "Unsupported WAVEFORMATEXTENSIBLE subtype!");
		}
	}
	else if((*ppSourceVoice)->src.format->wFormatTag == FAUDIO_FORMAT_MSADPCM)
	{
		(*ppSourceVoice)->src.decode = ((*ppSourceVoice)->src.format->nChannels == 2) ?
			FAudio_INTERNAL_DecodeStereoMSADPCM :
			FAudio_INTERNAL_DecodeMonoMSADPCM;
	}
	else
	{
		FAudio_assert(0 && "Unsupported format tag!");
	}

	if ((*ppSourceVoice)->src.format->nChannels == 1)
	{
		(*ppSourceVoice)->src.resample = FAudio_INTERNAL_ResampleMono;
	}
	else if ((*ppSourceVoice)->src.format->nChannels == 2)
	{
		(*ppSourceVoice)->src.resample = FAudio_INTERNAL_ResampleStereo;
	}
	else
	{
		(*ppSourceVoice)->src.resample = FAudio_INTERNAL_ResampleGeneric;
	}

	(*ppSourceVoice)->src.curBufferOffset = 0;

	/* Sends/Effects */
	FAudioVoice_SetEffectChain(*ppSourceVoice, pEffectChain);
	FAudioVoice_SetOutputVoices(*ppSourceVoice, pSendList);

	/* Default Levels */
	(*ppSourceVoice)->volume = 1.0f;
	(*ppSourceVoice)->channelVolume = (float*) FAudio_malloc(
		sizeof(float) * (*ppSourceVoice)->outputChannels
	);
	for (i = 0; i < (*ppSourceVoice)->outputChannels; i += 1)
	{
		(*ppSourceVoice)->channelVolume[i] = 1.0f;
	}

	/* Filters */
	if (Flags & FAUDIO_VOICE_USEFILTER)
	{
		(*ppSourceVoice)->filterState = (FAudioFilterState*) FAudio_malloc(
			sizeof(FAudioFilterState) * (*ppSourceVoice)->src.format->nChannels
		);
		FAudio_zero(
			(*ppSourceVoice)->filterState,
			sizeof(FAudioFilterState) * (*ppSourceVoice)->src.format->nChannels
		);
	}

	/* Sample Storage */
	(*ppSourceVoice)->src.decodeSamples = (uint32_t) FAudio_ceil(
		audio->updateSize *
		(double) MaxFrequencyRatio *
		(double) (*ppSourceVoice)->src.format->nSamplesPerSec /
		(double) audio->master->master.inputSampleRate
	) + EXTRA_DECODE_PADDING * 2;
	FAudio_INTERNAL_ResizeDecodeCache(
		audio,
		(*ppSourceVoice)->src.decodeSamples * (*ppSourceVoice)->src.format->nChannels
	);

	/* Add to list, finally. */
	LinkedList_PrependEntry(
		&audio->sources,
		*ppSourceVoice,
		audio->sourceLock
	);
	FAudio_AddRef(audio);
	return 0;
}

uint32_t FAudio_CreateSubmixVoice(
	FAudio *audio,
	FAudioSubmixVoice **ppSubmixVoice,
	uint32_t InputChannels,
	uint32_t InputSampleRate,
	uint32_t Flags,
	uint32_t ProcessingStage,
	const FAudioVoiceSends *pSendList,
	const FAudioEffectChain *pEffectChain
) {
	uint32_t i;

	*ppSubmixVoice = (FAudioSubmixVoice*) FAudio_malloc(sizeof(FAudioVoice));
	FAudio_zero(*ppSubmixVoice, sizeof(FAudioSubmixVoice));
	(*ppSubmixVoice)->audio = audio;
	(*ppSubmixVoice)->type = FAUDIO_VOICE_SUBMIX;
	(*ppSubmixVoice)->flags = Flags;
	(*ppSubmixVoice)->filter.Type = FAUDIO_DEFAULT_FILTER_TYPE;
	(*ppSubmixVoice)->filter.Frequency = FAUDIO_DEFAULT_FILTER_FREQUENCY;
	(*ppSubmixVoice)->filter.OneOverQ = FAUDIO_DEFAULT_FILTER_ONEOVERQ;
	(*ppSubmixVoice)->sendLock = FAudio_PlatformCreateMutex();
	(*ppSubmixVoice)->effectLock = FAudio_PlatformCreateMutex();
	(*ppSubmixVoice)->filterLock = FAudio_PlatformCreateMutex();
	(*ppSubmixVoice)->volumeLock = FAudio_PlatformCreateMutex();

	/* Submix Properties */
	(*ppSubmixVoice)->mix.inputChannels = InputChannels;
	(*ppSubmixVoice)->mix.inputSampleRate = InputSampleRate;
	(*ppSubmixVoice)->mix.processingStage = ProcessingStage;
	audio->submixStages = FAudio_max(
		audio->submixStages,
		ProcessingStage
	);

	/* Sends/Effects */
	FAudioVoice_SetEffectChain(*ppSubmixVoice, pEffectChain);
	FAudioVoice_SetOutputVoices(*ppSubmixVoice, pSendList);
	
	/* Default Levels */
	(*ppSubmixVoice)->volume = 1.0f;
	(*ppSubmixVoice)->channelVolume = (float*)FAudio_malloc(
		sizeof(float) * (*ppSubmixVoice)->outputChannels
	);
	for (i = 0; i < (*ppSubmixVoice)->outputChannels; i += 1)
	{
		(*ppSubmixVoice)->channelVolume[i] = 1.0f;
	}

	/* Filters */
	if (Flags & FAUDIO_VOICE_USEFILTER)
	{
		(*ppSubmixVoice)->filterState = (FAudioFilterState*) FAudio_malloc(
			sizeof(FAudioFilterState) * InputChannels
		);
		FAudio_zero(
			(*ppSubmixVoice)->filterState,
			sizeof(FAudioFilterState) * InputChannels
		);
	}

	/* Sample Storage */
	(*ppSubmixVoice)->mix.inputSamples = (uint32_t) FAudio_ceil(
		audio->updateSize *
		InputChannels *
		(double) InputSampleRate /
		(double) audio->master->master.inputSampleRate
	);
	(*ppSubmixVoice)->mix.inputCache = (float*) FAudio_malloc(
		sizeof(float) * (*ppSubmixVoice)->mix.inputSamples
	);
	FAudio_zero( /* Zero this now, for the first update */
		(*ppSubmixVoice)->mix.inputCache,
		sizeof(float) * (*ppSubmixVoice)->mix.inputSamples
	);

	/* Add to list, finally. */
	LinkedList_AddEntry(
		&audio->submixes,
		*ppSubmixVoice,
		audio->submixLock
	);
	FAudio_AddRef(audio);
	return 0;
}

uint32_t FAudio_CreateMasteringVoice(
	FAudio *audio,
	FAudioMasteringVoice **ppMasteringVoice,
	uint32_t InputChannels,
	uint32_t InputSampleRate,
	uint32_t Flags,
	uint32_t DeviceIndex,
	const FAudioEffectChain *pEffectChain
) {
	FAudioDeviceDetails details;

	/* For now we only support one allocated master voice at a time */
	FAudio_assert(audio->master == NULL);

	*ppMasteringVoice = (FAudioMasteringVoice*) FAudio_malloc(sizeof(FAudioVoice));
	FAudio_zero(*ppMasteringVoice, sizeof(FAudioMasteringVoice));
	(*ppMasteringVoice)->audio = audio;
	(*ppMasteringVoice)->type = FAUDIO_VOICE_MASTER;
	(*ppMasteringVoice)->flags = Flags;
	(*ppMasteringVoice)->effectLock = FAudio_PlatformCreateMutex();
	(*ppMasteringVoice)->volumeLock = FAudio_PlatformCreateMutex();

	/* Default Levels */
	(*ppMasteringVoice)->volume = 1.0f;

	/* Master Properties */
	FAudio_GetDeviceDetails(audio, DeviceIndex, &details);
	(*ppMasteringVoice)->master.inputChannels = (InputChannels == FAUDIO_DEFAULT_CHANNELS) ?
		details.OutputFormat.Format.nChannels :
		InputChannels;
	(*ppMasteringVoice)->master.inputSampleRate = (InputSampleRate == FAUDIO_DEFAULT_SAMPLERATE) ?
		details.OutputFormat.Format.nSamplesPerSec :
		InputSampleRate;

	/* Sends/Effects */
	FAudio_zero(&(*ppMasteringVoice)->sends, sizeof(FAudioVoiceSends));
	FAudioVoice_SetEffectChain(*ppMasteringVoice, pEffectChain);

	/* Platform Device */
	audio->master = *ppMasteringVoice;
	FAudio_AddRef(audio);
	FAudio_PlatformInit(audio, DeviceIndex);
	if (audio->active)
	{
		FAudio_PlatformStart(audio);
	}
	return 0;
}

uint32_t FAudio_StartEngine(FAudio *audio)
{
	if (!audio->active)
	{
		audio->active = 1;
		if (audio->master != NULL)
		{
			FAudio_PlatformStart(audio);
		}
	}
	return 0;
}

void FAudio_StopEngine(FAudio *audio)
{
	if (audio->active)
	{
		audio->active = 0;
		if (audio->master != NULL)
		{
			FAudio_PlatformStop(audio);
		}
	}
}

uint32_t FAudio_CommitChanges(FAudio *audio)
{
	FAudio_assert(0 && "Batching is not supported!");
	return 0;
}

void FAudio_GetPerformanceData(
	FAudio *audio,
	FAudioPerformanceData *pPerfData
) {
	FAudio_assert(0 && "TODO: Performance metrics!");
}

void FAudio_SetDebugConfiguration(
	FAudio *audio,
	FAudioDebugConfiguration *pDebugConfiguration,
	void* pReserved
) {
	FAudio_assert(0 && "TODO: Debug configuration!");
}

/* FAudioVoice Interface */

void FAudioVoice_GetVoiceDetails(
	FAudioVoice *voice,
	FAudioVoiceDetails *pVoiceDetails
) {
	pVoiceDetails->CreationFlags = voice->flags;
	pVoiceDetails->ActiveFlags = voice->flags;
	if (voice->type == FAUDIO_VOICE_SOURCE)
	{
		pVoiceDetails->InputChannels = voice->src.format->nChannels;
		pVoiceDetails->InputSampleRate = voice->src.format->nSamplesPerSec;
	}
	else if (voice->type == FAUDIO_VOICE_SUBMIX)
	{
		pVoiceDetails->InputChannels = voice->mix.inputChannels;
		pVoiceDetails->InputSampleRate = voice->mix.inputSampleRate;
	}
	else if (voice->type == FAUDIO_VOICE_MASTER)
	{
		pVoiceDetails->InputChannels = voice->master.inputChannels;
		pVoiceDetails->InputSampleRate = voice->master.inputSampleRate;
	}
	else
	{
		FAudio_assert(0 && "Unknown voice type!");
	}
}

uint32_t FAudioVoice_SetOutputVoices(
	FAudioVoice *voice,
	const FAudioVoiceSends *pSendList
) {
	uint32_t i;
	uint32_t outChannels;
	uint32_t outSampleRate;
	uint32_t newResampleSamples;
	FAudioVoiceSends defaultSends;
	FAudioSendDescriptor defaultSend;
	FAudio_assert(voice->type != FAUDIO_VOICE_MASTER);

	FAudio_PlatformLockMutex(voice->sendLock);

	/* FIXME: This is lazy... */
	for (i = 0; i < voice->sends.SendCount; i += 1)
	{
		FAudio_free(voice->sendCoefficients[i]);
	}
	if (voice->sendCoefficients != NULL)
	{
		FAudio_free(voice->sendCoefficients);
	}
	if (voice->sends.pSends != NULL)
	{
		FAudio_free(voice->sends.pSends);
	}

	if (pSendList == NULL)
	{
		/* Default to the mastering voice as output */
		defaultSend.Flags = 0;
		defaultSend.pOutputVoice = voice->audio->master;
		defaultSends.SendCount = 1;
		defaultSends.pSends = &defaultSend;
		pSendList = &defaultSends;
	}
	else if (pSendList->SendCount == 0)
	{
		/* No sends? Nothing to do... */
		FAudio_zero(&voice->sends, sizeof(FAudioVoiceSends));
		FAudio_PlatformUnlockMutex(voice->sendLock);
		return 0;
	}

	/* Copy send list */
	voice->sends.SendCount = pSendList->SendCount;
	voice->sends.pSends = (FAudioSendDescriptor*) FAudio_malloc(
		pSendList->SendCount * sizeof(FAudioSendDescriptor)
	);
	FAudio_memcpy(
		voice->sends.pSends,
		pSendList->pSends,
		pSendList->SendCount * sizeof(FAudioSendDescriptor)
	);

	/* Allocate/Reset default output matrix */
	voice->sendCoefficients = (float**) FAudio_malloc(
		sizeof(float*) * pSendList->SendCount
	);
	for (i = 0; i < pSendList->SendCount; i += 1)
	{
		if (pSendList->pSends[i].pOutputVoice->type == FAUDIO_VOICE_MASTER)
		{
			outChannels = pSendList->pSends[i].pOutputVoice->master.inputChannels;
		}
		else
		{
			outChannels = pSendList->pSends[i].pOutputVoice->mix.inputChannels;
		}
		voice->sendCoefficients[i] = (float*) FAudio_malloc(
			sizeof(float) * voice->outputChannels * outChannels
		);
		FAudio_INTERNAL_SetDefaultMatrix(
			voice->sendCoefficients[i],
			voice->outputChannels,
			outChannels
		);
	}

	/* Allocate resample cache */
	outSampleRate = voice->sends.pSends[0].pOutputVoice->type == FAUDIO_VOICE_MASTER ?
		voice->sends.pSends[0].pOutputVoice->master.inputSampleRate :
		voice->sends.pSends[0].pOutputVoice->mix.inputSampleRate;
	newResampleSamples = (uint32_t) FAudio_ceil(
		voice->audio->updateSize *
		(double) outSampleRate /
		(double) voice->audio->master->master.inputSampleRate
	);
	FAudio_INTERNAL_ResizeResampleCache(
		voice->audio,
		newResampleSamples * voice->outputChannels
	);
	if (voice->type == FAUDIO_VOICE_SOURCE)
	{
		voice->src.resampleSamples = newResampleSamples;
	}
	else
	{
		voice->mix.outputSamples = newResampleSamples;

		/* Init fixed-rate SRC if applicable */
		if (voice->mix.resampler != NULL)
		{
			/* FIXME: This is lazy... */
			FAudio_PlatformCloseFixedRateSRC(voice->mix.resampler);
		}
		voice->mix.resampler = FAudio_PlatformInitFixedRateSRC(
			voice->mix.inputChannels,
			voice->mix.inputSampleRate,
			outSampleRate
		);
	}

	FAudio_PlatformUnlockMutex(voice->sendLock);
	return 0;
}

uint32_t FAudioVoice_SetEffectChain(
	FAudioVoice *voice,
	const FAudioEffectChain *pEffectChain
) {
	uint32_t i;
	uint32_t channelCount;
	FAudioVoiceDetails voiceDetails;

	FAudioVoice_GetVoiceDetails(voice, &voiceDetails);

	/* SetEffectChain must not change the number of output channels once the voice has been created */
	if (pEffectChain == NULL && voice->outputChannels != 0)
	{
		/* cannot remove an effect chain that changes the number of channels */
		if (voice->outputChannels != voiceDetails.InputChannels)
		{
			FAudio_assert(0 && "Cannot remove effect chain that changes the number of channels");
			return FAUDIO_E_INVALID_CALL;
		}
	}

	if (pEffectChain != NULL && voice->outputChannels != 0)
	{
		uint32_t lst = pEffectChain->EffectCount - 1;

		/* new effect chain must have same number of output channels */
		if (voice->outputChannels != pEffectChain->pEffectDescriptors[lst].OutputChannels)
		{
			FAudio_assert(0 && "New effect chain must have same number of output channels as the old chain");
			return FAUDIO_E_INVALID_CALL;
		}
	}

	FAudio_PlatformLockMutex(voice->effectLock);

	if (pEffectChain == NULL)
	{
		FAudio_INTERNAL_FreeEffectChain(voice);
		FAudio_zero(&voice->effects, sizeof(voice->effects));
		voice->outputChannels = voiceDetails.InputChannels;
	}
	else
	{
		/* validate incoming effect chain before changing the current chain */
		for (i = 0; i < pEffectChain->EffectCount; i += 1)
		{
			FAPO *fapo = pEffectChain->pEffectDescriptors[i].pEffect;
			FAudioWaveFormatEx srcFmt, dstFmt;

			srcFmt.wBitsPerSample = 32;
			srcFmt.wFormatTag = FAUDIO_FORMAT_IEEE_FLOAT;
			srcFmt.nChannels = voiceDetails.InputChannels;
			srcFmt.nSamplesPerSec = voiceDetails.InputSampleRate;
			srcFmt.nBlockAlign = srcFmt.nChannels * (srcFmt.wBitsPerSample / 8);
			srcFmt.nAvgBytesPerSec = srcFmt.nSamplesPerSec * srcFmt.nBlockAlign;
			srcFmt.cbSize = 0;

			FAudio_memcpy(&dstFmt, &srcFmt, sizeof(srcFmt));
			dstFmt.nChannels = pEffectChain->pEffectDescriptors[i].OutputChannels;
			dstFmt.nBlockAlign = dstFmt.nChannels * (dstFmt.wBitsPerSample / 8);
			dstFmt.nAvgBytesPerSec = dstFmt.nSamplesPerSec * dstFmt.nBlockAlign;

			if (fapo->IsOutputFormatSupported(fapo, &srcFmt, &dstFmt, NULL))
			{
				FAudio_assert(0 && "Effect: output format not supported");
				FAudio_PlatformUnlockMutex(voice->effectLock);
				return FAUDIO_E_UNSUPPORTED_FORMAT;
			}
		}

		FAudio_INTERNAL_FreeEffectChain(voice);
		FAudio_INTERNAL_AllocEffectChain(
			voice,
			pEffectChain
		);

		/* check if in-place processing is supported */
		channelCount = voiceDetails.InputChannels;
		for (i = 0; i < voice->effects.count; i += 1)
		{
			FAPORegistrationProperties *pProps;
			FAPO *fapo = voice->effects.desc[i].pEffect;
			uint32_t r;

			r = fapo->GetRegistrationProperties(fapo, &pProps);
			if (r == 0)
			{
				voice->effects.inPlaceProcessing[i] = (pProps->Flags & FAPO_FLAG_INPLACE_SUPPORTED) == FAPO_FLAG_INPLACE_SUPPORTED;
				voice->effects.inPlaceProcessing[i] &= (channelCount == voice->effects.desc[i].OutputChannels);
				channelCount = voice->effects.desc[i].OutputChannels;
				FAudio_free(pProps);
			}
		}
		voice->outputChannels = channelCount;
	}

	FAudio_PlatformUnlockMutex(voice->effectLock);
	return 0;
}

uint32_t FAudioVoice_EnableEffect(
	FAudioVoice *voice,
	uint32_t EffectIndex,
	uint32_t OperationSet
) {
	FAudio_assert(OperationSet == FAUDIO_COMMIT_NOW);

	FAudio_PlatformLockMutex(voice->effectLock);
	voice->effects.desc[EffectIndex].InitialState = 1;
	FAudio_PlatformUnlockMutex(voice->effectLock);
	return 0;
}

uint32_t FAudioVoice_DisableEffect(
	FAudioVoice *voice,
	uint32_t EffectIndex,
	uint32_t OperationSet
) {
	FAudio_assert(OperationSet == FAUDIO_COMMIT_NOW);

	FAudio_PlatformLockMutex(voice->effectLock);
	voice->effects.desc[EffectIndex].InitialState = 0;
	FAudio_PlatformUnlockMutex(voice->effectLock);
	return 0;
}

void FAudioVoice_GetEffectState(
	FAudioVoice *voice,
	uint32_t EffectIndex,
	uint8_t *pEnabled
) {
	FAudio_PlatformLockMutex(voice->effectLock);
	*pEnabled = voice->effects.desc[EffectIndex].InitialState;
	FAudio_PlatformUnlockMutex(voice->effectLock);
}

uint32_t FAudioVoice_SetEffectParameters(
	FAudioVoice *voice,
	uint32_t EffectIndex,
	const void *pParameters,
	uint32_t ParametersByteSize,
	uint32_t OperationSet
) {
	FAudio_assert(OperationSet == FAUDIO_COMMIT_NOW);

	if (voice->effects.parameters[EffectIndex] == NULL)
	{
		voice->effects.parameters[EffectIndex] = FAudio_malloc(
			ParametersByteSize
		);
		voice->effects.parameterSizes[EffectIndex] = ParametersByteSize;
	}
	FAudio_PlatformLockMutex(voice->effectLock);
	if (voice->effects.parameterSizes[EffectIndex] < ParametersByteSize)
	{
		voice->effects.parameters[EffectIndex] = FAudio_realloc(
			voice->effects.parameters[EffectIndex],
			ParametersByteSize
		);
		voice->effects.parameterSizes[EffectIndex] = ParametersByteSize;
	}
	FAudio_memcpy(
		voice->effects.parameters[EffectIndex],
		pParameters,
		ParametersByteSize
	);
	voice->effects.parameterUpdates[EffectIndex] = 1;
	FAudio_PlatformUnlockMutex(voice->effectLock);
	return 0;
}

uint32_t FAudioVoice_GetEffectParameters(
	FAudioVoice *voice,
	uint32_t EffectIndex,
	void *pParameters,
	uint32_t ParametersByteSize
) {
	FAPO *fapo;
	FAudio_PlatformLockMutex(voice->effectLock);
	fapo = voice->effects.desc[EffectIndex].pEffect;
	fapo->GetParameters(fapo, pParameters, ParametersByteSize);
	FAudio_PlatformUnlockMutex(voice->effectLock);
	return 0;
}

uint32_t FAudioVoice_SetFilterParameters(
	FAudioVoice *voice,
	const FAudioFilterParameters *pParameters,
	uint32_t OperationSet
) {
	FAudio_assert(OperationSet == FAUDIO_COMMIT_NOW);

	/* MSDN: "This method is usable only on source and submix voices and
	 * has no effect on mastering voices."
	 */
	if (voice->type == FAUDIO_VOICE_MASTER)
	{
		return 0;
	}

	if (!(voice->flags & FAUDIO_VOICE_USEFILTER))
	{
		return 0;
	}

	FAudio_PlatformLockMutex(voice->filterLock);
	FAudio_memcpy(
		&voice->filter,
		pParameters,
		sizeof(FAudioFilterParameters)
	);
	FAudio_PlatformUnlockMutex(voice->filterLock);

	return 0;
}

void FAudioVoice_GetFilterParameters(
	FAudioVoice *voice,
	FAudioFilterParameters *pParameters
) {
	FAudio_PlatformLockMutex(voice->filterLock);
	FAudio_memcpy(
		pParameters,
		&voice->filter,
		sizeof(FAudioFilterParameters)
	);
	FAudio_PlatformUnlockMutex(voice->filterLock);
}

uint32_t FAudioVoice_SetOutputFilterParameters(
	FAudioVoice *voice,
	FAudioVoice *pDestinationVoice,
	const FAudioFilterParameters *pParameters,
	uint32_t OperationSet
) {
	FAudio_assert(OperationSet == FAUDIO_COMMIT_NOW);
	FAudio_assert(0 && "Output filters are not supported!");

	if (!(voice->flags & FAUDIO_VOICE_USEFILTER))
	{
		return 0;
	}
	return 0;
}

void FAudioVoice_GetOutputFilterParameters(
	FAudioVoice *voice,
	FAudioVoice *pDestinationVoice,
	FAudioFilterParameters *pParameters
) {
	FAudio_assert(0 && "Output filters are not supported!");
}

uint32_t FAudioVoice_SetVolume(
	FAudioVoice *voice,
	float Volume,
	uint32_t OperationSet
) {
	FAudio_assert(OperationSet == FAUDIO_COMMIT_NOW);

	voice->volume = FAudio_clamp(
		Volume,
		-FAUDIO_MAX_VOLUME_LEVEL,
		FAUDIO_MAX_VOLUME_LEVEL
	);
	return 0;
}

void FAudioVoice_GetVolume(
	FAudioVoice *voice,
	float *pVolume
) {
	*pVolume = voice->volume;
}

uint32_t FAudioVoice_SetChannelVolumes(
	FAudioVoice *voice,
	uint32_t Channels,
	const float *pVolumes,
	uint32_t OperationSet
) {
	FAudio_assert(voice->type != FAUDIO_VOICE_MASTER);
	FAudio_assert(OperationSet == FAUDIO_COMMIT_NOW);

	if (!pVolumes)
	{
		return FAUDIO_E_INVALID_CALL;
	}

	if (voice->audio->version > 7 && Channels != voice->outputChannels)
	{
		return FAUDIO_E_INVALID_CALL;
	}

	FAudio_PlatformLockMutex(voice->volumeLock);
	FAudio_memcpy(
		voice->channelVolume,
		pVolumes,
		sizeof(float) * Channels
	);
	FAudio_PlatformUnlockMutex(voice->volumeLock);
	return 0;
}

void FAudioVoice_GetChannelVolumes(
	FAudioVoice *voice,
	uint32_t Channels,
	float *pVolumes
) {
	FAudio_PlatformLockMutex(voice->volumeLock);
	FAudio_memcpy(
		pVolumes,
		voice->channelVolume,
		sizeof(float) * Channels
	);
	FAudio_PlatformUnlockMutex(voice->volumeLock);
}

uint32_t FAudioVoice_SetOutputMatrix(
	FAudioVoice *voice,
	FAudioVoice *pDestinationVoice,
	uint32_t SourceChannels,
	uint32_t DestinationChannels,
	const float *pLevelMatrix,
	uint32_t OperationSet
) {
	uint32_t i;
	FAudio_assert(OperationSet == FAUDIO_COMMIT_NOW);

	FAudio_PlatformLockMutex(voice->sendLock);

	/* Find the send index */
	if (pDestinationVoice == NULL && voice->sends.SendCount == 1)
	{
		pDestinationVoice = voice->audio->master;
	}
	for (i = 0; i < voice->sends.SendCount; i += 1)
	{
		if (pDestinationVoice == voice->sends.pSends[i].pOutputVoice)
		{
			break;
		}
	}
	FAudio_assert(i < voice->sends.SendCount);

	/* Verify the Source/Destination channel count */
	FAudio_assert(SourceChannels == voice->outputChannels);

	if (pDestinationVoice->type == FAUDIO_VOICE_MASTER)
	{
		FAudio_assert(DestinationChannels == pDestinationVoice->master.inputChannels);
	}
	else
	{
		FAudio_assert(DestinationChannels == pDestinationVoice->mix.inputChannels);
	}

	/* Set the matrix values, finally */
	FAudio_memcpy(
		voice->sendCoefficients[i],
		pLevelMatrix,
		sizeof(float) * SourceChannels * DestinationChannels
	);

	FAudio_PlatformUnlockMutex(voice->sendLock);
	return 0;
}

void FAudioVoice_GetOutputMatrix(
	FAudioVoice *voice,
	FAudioVoice *pDestinationVoice,
	uint32_t SourceChannels,
	uint32_t DestinationChannels,
	float *pLevelMatrix
) {
	uint32_t i;

	FAudio_PlatformLockMutex(voice->sendLock);

	/* Find the send index */
	for (i = 0; i < voice->sends.SendCount; i += 1)
	{
		if (pDestinationVoice == voice->sends.pSends[i].pOutputVoice)
		{
			break;
		}
	}
	FAudio_assert(i < voice->sends.SendCount);

	/* Verify the Source/Destination channel count */
	if (voice->type == FAUDIO_VOICE_SOURCE)
	{
		FAudio_assert(SourceChannels == voice->src.format->nChannels);
	}
	else
	{
		FAudio_assert(SourceChannels == voice->mix.inputChannels);
	}
	if (pDestinationVoice->type == FAUDIO_VOICE_MASTER)
	{
		FAudio_assert(DestinationChannels == pDestinationVoice->master.inputChannels);
	}
	else
	{
		FAudio_assert(DestinationChannels == voice->mix.inputChannels);
	}

	/* Get the matrix values, finally */
	FAudio_memcpy(
		pLevelMatrix,
		voice->sendCoefficients[i],
		sizeof(float) * SourceChannels * DestinationChannels
	);

	FAudio_PlatformUnlockMutex(voice->sendLock);
}

void FAudioVoice_DestroyVoice(FAudioVoice *voice)
{
	uint32_t i;
	LinkedList *list;
	FAudioSubmixVoice *submix;

	/* TODO: Check for dependencies and fail if still in use */
	if (voice->type == FAUDIO_VOICE_SOURCE)
	{
		LinkedList_RemoveEntry(
			&voice->audio->sources,
			voice,
			voice->audio->sourceLock
		);
		FAudio_free(voice->src.format);
		FAudio_PlatformDestroyMutex(voice->src.bufferLock);
	}
	else if (voice->type == FAUDIO_VOICE_SUBMIX)
	{
		/* Remove submix from list */
		LinkedList_RemoveEntry(
			&voice->audio->submixes,
			voice,
			voice->audio->submixLock
		);

		/* Check submix stage count */
		voice->audio->submixStages = 0;
		list = voice->audio->submixes;
		while (list != NULL)
		{
			submix = (FAudioSubmixVoice*) list->entry;
			voice->audio->submixStages = FAudio_max(
				voice->audio->submixStages,
				submix->mix.processingStage
			);
			list = list->next;
		}

		/* Delete submix data */
		FAudio_free(voice->mix.inputCache);
		if (voice->mix.resampler != NULL)
		{
			FAudio_PlatformCloseFixedRateSRC(voice->mix.resampler);
		}
	}
	else if (voice->type == FAUDIO_VOICE_MASTER)
	{
		FAudio_PlatformQuit(voice->audio);
		voice->audio->master = NULL;
	}

	if (voice->sendLock != NULL)
	{
		FAudio_PlatformLockMutex(voice->sendLock);
		for (i = 0; i < voice->sends.SendCount; i += 1)
		{
			FAudio_free(voice->sendCoefficients[i]);
		}
		if (voice->sendCoefficients != NULL)
		{
			FAudio_free(voice->sendCoefficients);
		}
		if (voice->sends.pSends != NULL)
		{
			FAudio_free(voice->sends.pSends);
		}
		FAudio_PlatformUnlockMutex(voice->sendLock);
		FAudio_PlatformDestroyMutex(voice->sendLock);
	}

	if (voice->effectLock != NULL)
	{
		FAudio_PlatformLockMutex(voice->effectLock);
		if (voice->effects.desc != NULL)
		{
			for (i = 0; i < voice->effects.count; i += 1)
			{
				voice->effects.desc[i].pEffect->Release(voice->effects.desc[i].pEffect);
			}
			FAudio_free(voice->effects.parameters);
			FAudio_free(voice->effects.parameterSizes);
			FAudio_free(voice->effects.parameterUpdates);
			FAudio_free(voice->effects.desc);
		}
		FAudio_PlatformUnlockMutex(voice->effectLock);
		FAudio_PlatformDestroyMutex(voice->effectLock);
	}

	if (voice->filterLock != NULL)
	{
		FAudio_PlatformLockMutex(voice->filterLock);
		if (voice->filterState != NULL)
		{
			FAudio_free(voice->filterState);
		}
		FAudio_PlatformUnlockMutex(voice->filterLock);
		FAudio_PlatformDestroyMutex(voice->filterLock);
	}

	if (voice->volumeLock != NULL)
	{
		FAudio_PlatformLockMutex(voice->volumeLock);
		if (voice->channelVolume != NULL)
		{
			FAudio_free(voice->channelVolume);
		}
		FAudio_PlatformUnlockMutex(voice->volumeLock);
		FAudio_PlatformDestroyMutex(voice->volumeLock);
	}

	FAudio_Release(voice->audio);
	FAudio_free(voice);
}

/* FAudioSourceVoice Interface */

uint32_t FAudioSourceVoice_Start(
	FAudioSourceVoice *voice,
	uint32_t Flags,
	uint32_t OperationSet
) {
	FAudio_assert(OperationSet == FAUDIO_COMMIT_NOW);
	FAudio_assert(voice->type == FAUDIO_VOICE_SOURCE);

	FAudio_assert(Flags == 0);
	voice->src.active = 1;
	return 0;
}

uint32_t FAudioSourceVoice_Stop(
	FAudioSourceVoice *voice,
	uint32_t Flags,
	uint32_t OperationSet
) {
	FAudio_assert(OperationSet == FAUDIO_COMMIT_NOW);
	FAudio_assert(voice->type == FAUDIO_VOICE_SOURCE);

	FAudio_assert(!(Flags & FAUDIO_PLAY_TAILS)); /* FIXME: ??? */
	voice->src.active = 0;
	return 0;
}

uint32_t FAudioSourceVoice_SubmitSourceBuffer(
	FAudioSourceVoice *voice,
	const FAudioBuffer *pBuffer,
	const FAudioBufferWMA *pBufferWMA
) {
	uint32_t adpcmMask, *adpcmByteCount;
	uint32_t playBegin, playLength, loopBegin, loopLength;
	FAudioBufferEntry *entry, *list;
	FAudio_assert(voice->type == FAUDIO_VOICE_SOURCE);
	FAudio_assert(pBufferWMA == NULL);

	/* Start off with whatever they just sent us... */
	playBegin = pBuffer->PlayBegin;
	playLength = pBuffer->PlayLength;
	loopBegin = pBuffer->LoopBegin;
	loopLength = pBuffer->LoopLength;

	/* "LoopBegin/LoopLength must be zero if LoopCount is 0" */
	if (pBuffer->LoopCount == 0 && (loopBegin > 0 || loopLength > 0))
	{
		return FAUDIO_E_INVALID_CALL;
	}

	/* PlayLength Default */
	if (playLength == 0)
	{
		if (voice->src.format->wFormatTag == FAUDIO_FORMAT_MSADPCM)
		{
			playLength = (
				pBuffer->AudioBytes /
				voice->src.format->nBlockAlign *
				(((voice->src.format->nBlockAlign / voice->src.format->nChannels) - 6) * 2)
			) - playBegin;
		}
		else
		{
			playLength = (
				pBuffer->AudioBytes /
				voice->src.format->nBlockAlign
			) - playBegin;
		}
	}

	if (pBuffer->LoopCount > 0)
	{
		/* "The value of LoopBegin must be less than PlayBegin + PlayLength" */
		if (loopBegin >= (playBegin + playLength))
		{
			return FAUDIO_E_INVALID_CALL;
		}

		/* LoopLength Default */
		if (loopLength == 0)
		{
			loopLength = playBegin + playLength - loopBegin;
		}

		/* "The value of LoopBegin + LoopLength must be greater than PlayBegin
		 * and less than PlayBegin + PlayLength"
		 */
		if (	voice->audio->version > 7 && (
			(loopBegin + loopLength) <= playBegin ||
			(loopBegin + loopLength) > (playBegin + playLength))	)
		{
			return FAUDIO_E_INVALID_CALL;
		}
	}

	/* For ADPCM, round down to the nearest sample block size */
	if (voice->src.format->wFormatTag == FAUDIO_FORMAT_MSADPCM)
	{
		adpcmMask = ((voice->src.format->nBlockAlign / voice->src.format->nChannels) - 6) * 2;
		adpcmMask -= 1;
		playBegin &= ~adpcmMask;
		playLength &= ~adpcmMask;
		loopBegin &= ~adpcmMask;
		loopLength &= ~adpcmMask;

		/* This is basically a const_cast... */
		adpcmByteCount = (uint32_t*) &pBuffer->AudioBytes;
		*adpcmByteCount = (
			pBuffer->AudioBytes / voice->src.format->nBlockAlign
		) * voice->src.format->nBlockAlign;
	}

	/* Allocate, now that we have valid input */
	entry = (FAudioBufferEntry*) FAudio_malloc(sizeof(FAudioBufferEntry));
	FAudio_memcpy(&entry->buffer, pBuffer, sizeof(FAudioBuffer));
	entry->buffer.PlayBegin = playBegin;
	entry->buffer.PlayLength = playLength;
	entry->buffer.LoopBegin = loopBegin;
	entry->buffer.LoopLength = loopLength;
	entry->next = NULL;

	if (	voice->audio->version <= 7 && (
		entry->buffer.LoopCount > 0 &&
		entry->buffer.LoopBegin + entry->buffer.LoopLength <= entry->buffer.PlayBegin))
	{
		entry->buffer.LoopCount = 0;
	}

	/* Submit! */
	FAudio_PlatformLockMutex(voice->src.bufferLock);
	if (voice->src.bufferList == NULL)
	{
		voice->src.bufferList = entry;
		voice->src.curBufferOffset = entry->buffer.PlayBegin;
	}
	else
	{
		list = voice->src.bufferList;
		while (list->next != NULL)
		{
			list = list->next;
		}
		list->next = entry;

		/* For some bizarre reason we get scenarios where a buffer is freed, only to
		 * have the allocator give us the exact same address and somehow get a single
		 * buffer referencing itself. I don't even know.
		 */
		FAudio_assert(list != entry);
	}
	FAudio_PlatformUnlockMutex(voice->src.bufferLock);
	return 0;
}

uint32_t FAudioSourceVoice_FlushSourceBuffers(
	FAudioSourceVoice *voice
) {
	FAudioBufferEntry *entry, *next;
	FAudio_assert(voice->type == FAUDIO_VOICE_SOURCE);

	FAudio_PlatformLockMutex(voice->src.bufferLock);

	/* If the source is playing, don't flush the active buffer */
	entry = voice->src.bufferList;
	if (voice->src.active && entry != NULL)
	{
		entry = entry->next;
		voice->src.bufferList->next = NULL;
	}
	else
	{
		voice->src.curBufferOffset = 0;
		voice->src.bufferList = NULL;
	}

	/* Go through each buffer, send an event for each one before deleting */
	while (entry != NULL)
	{
		if (voice->src.callback != NULL && voice->src.callback->OnBufferEnd != NULL)
		{
			voice->src.callback->OnBufferEnd(
				voice->src.callback,
				entry->buffer.pContext
			);
		}
		next = entry->next;
		FAudio_free(entry);
		entry = next;
	}

	FAudio_PlatformUnlockMutex(voice->src.bufferLock);
	return 0;
}

uint32_t FAudioSourceVoice_Discontinuity(
	FAudioSourceVoice *voice
) {
	FAudio_assert(voice->type == FAUDIO_VOICE_SOURCE);

	/* As far as I know this doesn't matter for us...?
	 * This exists so the engine doesn't try to spit out random memory,
	 * but like... can't we just not send samples with no buffers?
	 * -flibit
	 */
	return 0;
}

uint32_t FAudioSourceVoice_ExitLoop(
	FAudioSourceVoice *voice,
	uint32_t OperationSet
) {
	FAudio_assert(OperationSet == FAUDIO_COMMIT_NOW);
	FAudio_assert(voice->type == FAUDIO_VOICE_SOURCE);

	FAudio_PlatformLockMutex(voice->src.bufferLock);

	if (voice->src.bufferList != NULL)
	{
		voice->src.bufferList->buffer.LoopCount = 0;
	}

	FAudio_PlatformUnlockMutex(voice->src.bufferLock);
	return 0;
}

void FAudioSourceVoice_GetState(
	FAudioSourceVoice *voice,
	FAudioVoiceState *pVoiceState,
	uint32_t flags
) {
	FAudioBufferEntry *entry;
	FAudio_assert(voice->type == FAUDIO_VOICE_SOURCE);

	FAudio_PlatformLockMutex(voice->src.bufferLock);

	if (!(flags & FAUDIO_VOICE_NOSAMPLESPLAYED))
	{
		pVoiceState->SamplesPlayed = voice->src.totalSamples;
	}

	pVoiceState->BuffersQueued = 0;
	if (voice->src.bufferList != NULL)
	{
		entry = voice->src.bufferList;
		pVoiceState->pCurrentBufferContext = entry->buffer.pContext;
		do
		{
			pVoiceState->BuffersQueued += 1;
			entry = entry->next;
		} while (entry != NULL);
	}
	else
	{
		pVoiceState->pCurrentBufferContext = NULL;
	}

	FAudio_PlatformUnlockMutex(voice->src.bufferLock);
}

uint32_t FAudioSourceVoice_SetFrequencyRatio(
	FAudioSourceVoice *voice,
	float Ratio,
	uint32_t OperationSet
) {
	FAudio_assert(OperationSet == FAUDIO_COMMIT_NOW);
	FAudio_assert(voice->type == FAUDIO_VOICE_SOURCE);

	if (voice->flags & FAUDIO_VOICE_NOPITCH)
	{
		return 0;
	}

	voice->src.freqRatio = FAudio_clamp(
		Ratio,
		FAUDIO_MIN_FREQ_RATIO,
		voice->src.maxFreqRatio
	);
	return 0;
}

void FAudioSourceVoice_GetFrequencyRatio(
	FAudioSourceVoice *voice,
	float *pRatio
) {
	FAudio_assert(voice->type == FAUDIO_VOICE_SOURCE);

	*pRatio = voice->src.freqRatio;
}

uint32_t FAudioSourceVoice_SetSourceSampleRate(
	FAudioSourceVoice *voice,
	uint32_t NewSourceSampleRate
) {
	uint32_t outSampleRate;
	uint32_t newDecodeSamples, newResampleSamples;
	FAudio_assert(voice->type == FAUDIO_VOICE_SOURCE);

	FAudio_assert(	NewSourceSampleRate >= FAUDIO_MIN_SAMPLE_RATE &&
			NewSourceSampleRate <= FAUDIO_MAX_SAMPLE_RATE	);

	FAudio_PlatformLockMutex(voice->src.bufferLock);
	if (	voice->audio->version > 7 &&
		voice->src.bufferList != NULL	)
	{
		FAudio_PlatformUnlockMutex(voice->src.bufferLock);
		return FAUDIO_E_INVALID_CALL;
	}
	FAudio_PlatformUnlockMutex(voice->src.bufferLock);

	voice->src.format->nSamplesPerSec = NewSourceSampleRate;

	/* Resize decode cache */
	newDecodeSamples = (uint32_t) FAudio_ceil(
		voice->audio->updateSize *
		(double) voice->src.maxFreqRatio *
		(double) NewSourceSampleRate /
		(double) voice->audio->master->master.inputSampleRate
	) + EXTRA_DECODE_PADDING * 2;
	FAudio_INTERNAL_ResizeDecodeCache(
		voice->audio,
		newDecodeSamples * voice->src.format->nChannels
	);
	voice->src.decodeSamples = newDecodeSamples;

	FAudio_PlatformLockMutex(voice->sendLock);

	if (voice->sends.SendCount == 0)
	{
		FAudio_PlatformUnlockMutex(voice->sendLock);
		return 0;
	}
	outSampleRate = voice->sends.pSends[0].pOutputVoice->type == FAUDIO_VOICE_MASTER ?
		voice->sends.pSends[0].pOutputVoice->master.inputSampleRate :
		voice->sends.pSends[0].pOutputVoice->mix.inputSampleRate;

	FAudio_PlatformUnlockMutex(voice->sendLock);

	/* Resize resample cache */
	newResampleSamples = (uint32_t) FAudio_ceil(
		voice->audio->updateSize *
		(double) outSampleRate /
		(double) voice->audio->master->master.inputSampleRate
	);
	FAudio_INTERNAL_ResizeResampleCache(
		voice->audio,
		newResampleSamples * voice->src.format->nChannels
	);
	voice->src.resampleSamples = newResampleSamples;
	return 0;
}

/* FAudioMasteringVoice Interface */

FAUDIOAPI uint32_t FAudioMasteringVoice_GetChannelMask(
	FAudioMasteringVoice *voice,
	uint32_t *pChannelMask
) {
	FAudio_assert(voice->type == FAUDIO_VOICE_MASTER);
	FAudio_assert(voice->audio->mixFormat != NULL);
	FAudio_assert(pChannelMask != NULL);

	*pChannelMask = voice->audio->mixFormat->dwChannelMask;
	return 0;
}
