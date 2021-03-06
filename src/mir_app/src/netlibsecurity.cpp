/*

Miranda NG: the free IM client for Microsoft* Windows*

Copyright (�) 2012-16 Miranda NG project (http://miranda-ng.org),
Copyright (c) 2000-12 Miranda IM project,
all portions of this codebase are copyrighted to the people
listed in contributors.txt.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include "stdafx.h"
#include "netlib.h"

#define SECURITY_WIN32
#include <security.h>
#include <rpcdce.h>

#pragma comment(lib, "secur32.lib")

struct NtlmHandleType
{
	CtxtHandle hClientContext;
	CredHandle hClientCredential;
	wchar_t* szProvider;
	wchar_t* szPrincipal;
	unsigned cbMaxToken;
	bool hasDomain;
};

struct NTLM_String
{
	WORD     len;
	WORD     allocedSpace;
	DWORD    offset;
};

struct NtlmType2packet
{
	char        sign[8];
	DWORD       type;   // == 2
	NTLM_String targetName;
	DWORD       flags;
	BYTE        challenge[8];
	BYTE        context[8];
	NTLM_String targetInfo;
};

static unsigned ntlmCnt = 0;
static mir_cs csSec;

static void ReportSecError(SECURITY_STATUS scRet, int line)
{
	char szMsgBuf[256];
	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, scRet, LANG_USER_DEFAULT, szMsgBuf, _countof(szMsgBuf), NULL);

	char *p = strchr(szMsgBuf, 13); if (p) *p = 0;

	NetlibLogf(NULL, "Security error 0x%x on line %u (%s)", scRet, line, szMsgBuf);
}

HANDLE NetlibInitSecurityProvider(const wchar_t* szProvider, const wchar_t* szPrincipal)
{
	HANDLE hSecurity = NULL;

	if (mir_wstrcmpi(szProvider, L"Basic") == 0) {
		NtlmHandleType* hNtlm = (NtlmHandleType*)mir_calloc(sizeof(NtlmHandleType));
		hNtlm->szProvider = mir_wstrdup(szProvider);
		SecInvalidateHandle(&hNtlm->hClientContext);
		SecInvalidateHandle(&hNtlm->hClientCredential);
		ntlmCnt++;

		return hNtlm;
	}

	mir_cslock lck(csSec);

	PSecPkgInfo ntlmSecurityPackageInfo;
	bool isGSSAPI = mir_wstrcmpi(szProvider, L"GSSAPI") == 0;
	const wchar_t *szProviderC = isGSSAPI ? L"Kerberos" : szProvider;
	SECURITY_STATUS sc = QuerySecurityPackageInfo((LPTSTR)szProviderC, &ntlmSecurityPackageInfo);
	if (sc == SEC_E_OK) {
		NtlmHandleType* hNtlm;

		hSecurity = hNtlm = (NtlmHandleType*)mir_calloc(sizeof(NtlmHandleType));
		hNtlm->cbMaxToken = ntlmSecurityPackageInfo->cbMaxToken;
		FreeContextBuffer(ntlmSecurityPackageInfo);

		hNtlm->szProvider = mir_wstrdup(szProvider);
		hNtlm->szPrincipal = mir_wstrdup(szPrincipal ? szPrincipal : L"");
		SecInvalidateHandle(&hNtlm->hClientContext);
		SecInvalidateHandle(&hNtlm->hClientCredential);
		ntlmCnt++;
	}
	return hSecurity;
}

HANDLE NetlibInitSecurityProvider(const char* szProvider, const char* szPrincipal)
{
	return NetlibInitSecurityProvider(_A2T(szProvider), _A2T(szPrincipal));
}

void NetlibDestroySecurityProvider(HANDLE hSecurity)
{
	if (hSecurity == NULL)
		return;

	mir_cslock lck(csSec);

	if (ntlmCnt != 0) {
		NtlmHandleType* hNtlm = (NtlmHandleType*)hSecurity;
		if (hNtlm != NULL) {
			if (SecIsValidHandle(&hNtlm->hClientContext))
				DeleteSecurityContext(&hNtlm->hClientContext);
			if (SecIsValidHandle(&hNtlm->hClientCredential))
				FreeCredentialsHandle(&hNtlm->hClientCredential);
			mir_free(hNtlm->szProvider);
			mir_free(hNtlm->szPrincipal);
			mir_free(hNtlm);
		}

		--ntlmCnt;
	}
}

char* CompleteGssapi(HANDLE hSecurity, unsigned char *szChallenge, unsigned chlsz)
{
	if (!szChallenge || !szChallenge[0]) return NULL;

	NtlmHandleType* hNtlm = (NtlmHandleType*)hSecurity;
	unsigned char inDataBuffer[1024];

	SecBuffer inBuffers[2] =
	{
		{ sizeof(inDataBuffer), SECBUFFER_DATA, inDataBuffer },
		{ chlsz, SECBUFFER_STREAM, szChallenge }
	};

	SecBufferDesc inBuffersDesc = { SECBUFFER_VERSION, 2, inBuffers };

	unsigned long qop = 0;
	SECURITY_STATUS sc = DecryptMessage(&hNtlm->hClientContext, &inBuffersDesc, 0, &qop);
	if (sc != SEC_E_OK) {
		ReportSecError(sc, __LINE__);
		return NULL;
	}

	// unsigned char LayerMask = inDataBuffer[0];
	// unsigned int MaxMessageSize = htonl(*(unsigned*)&inDataBuffer[1]);

	SecPkgContext_Sizes sizes;
	sc = QueryContextAttributes(&hNtlm->hClientContext, SECPKG_ATTR_SIZES, &sizes);
	if (sc != SEC_E_OK) {
		ReportSecError(sc, __LINE__);
		return NULL;
	}

	unsigned char *tokenBuffer = (unsigned char*)alloca(sizes.cbSecurityTrailer);
	unsigned char *paddingBuffer = (unsigned char*)alloca(sizes.cbBlockSize);

	unsigned char outDataBuffer[4] = { 1, 0, 16, 0 };

	SecBuffer outBuffers[3] =
	{
		{ sizes.cbSecurityTrailer, SECBUFFER_TOKEN, tokenBuffer },
		{ sizeof(outDataBuffer), SECBUFFER_DATA, outDataBuffer },
		{ sizes.cbBlockSize, SECBUFFER_PADDING, paddingBuffer }
	};
	SecBufferDesc outBuffersDesc = { SECBUFFER_VERSION, 3, outBuffers };

	sc = EncryptMessage(&hNtlm->hClientContext, SECQOP_WRAP_NO_ENCRYPT, &outBuffersDesc, 0);
	if (sc != SEC_E_OK) {
		ReportSecError(sc, __LINE__);
		return NULL;
	}

	unsigned i, ressz = 0;
	for (i = 0; i < outBuffersDesc.cBuffers; i++)
		ressz += outBuffersDesc.pBuffers[i].cbBuffer;

	unsigned char *response = (unsigned char*)alloca(ressz), *p = response;
	for (i = 0; i < outBuffersDesc.cBuffers; i++) {
		memcpy(p, outBuffersDesc.pBuffers[i].pvBuffer, outBuffersDesc.pBuffers[i].cbBuffer);
		p += outBuffersDesc.pBuffers[i].cbBuffer;
	}

	return mir_base64_encode(response, ressz);
}

char* NtlmCreateResponseFromChallenge(HANDLE hSecurity, const char *szChallenge, const wchar_t* login, const wchar_t* psw, bool http, unsigned& complete)
{
	if (hSecurity == NULL || ntlmCnt == 0)
		return NULL;

	SecBufferDesc outputBufferDescriptor, inputBufferDescriptor;
	SecBuffer outputSecurityToken, inputSecurityToken;
	TimeStamp tokenExpiration;
	ULONG contextAttributes;
	char *szOutputToken;

	NtlmHandleType* hNtlm = (NtlmHandleType*)hSecurity;
	if (mir_wstrcmpi(hNtlm->szProvider, L"Basic")) {
		bool isGSSAPI = mir_wstrcmpi(hNtlm->szProvider, L"GSSAPI") == 0;
		wchar_t *szProvider = isGSSAPI ? (wchar_t*)L"Kerberos" : hNtlm->szProvider;
		bool hasChallenge = szChallenge != NULL && szChallenge[0] != '\0';
		if (hasChallenge) {
			unsigned tokenLen;
			BYTE *token = (BYTE*)mir_base64_decode(szChallenge, &tokenLen);
			if (token == NULL)
				return NULL;

			if (isGSSAPI && complete)
				return CompleteGssapi(hSecurity, token, tokenLen);

			inputBufferDescriptor.cBuffers = 1;
			inputBufferDescriptor.pBuffers = &inputSecurityToken;
			inputBufferDescriptor.ulVersion = SECBUFFER_VERSION;
			inputSecurityToken.BufferType = SECBUFFER_TOKEN;
			inputSecurityToken.cbBuffer = tokenLen;
			inputSecurityToken.pvBuffer = token;

			// try to decode the domain name from the NTLM challenge
			if (login != NULL && login[0] != '\0' && !hNtlm->hasDomain) {
				NtlmType2packet* pkt = (NtlmType2packet*)token;
				if (!strncmp(pkt->sign, "NTLMSSP", 8) && pkt->type == 2) {

					wchar_t* domainName = (wchar_t*)&token[pkt->targetName.offset];
					int domainLen = pkt->targetName.len;

					// Negotiate ANSI? if yes, convert the ANSI name to unicode
					if ((pkt->flags & 1) == 0) {
						int bufsz = MultiByteToWideChar(CP_ACP, 0, (char*)domainName, domainLen, NULL, 0);
						wchar_t* buf = (wchar_t*)alloca(bufsz * sizeof(wchar_t));
						domainLen = MultiByteToWideChar(CP_ACP, 0, (char*)domainName, domainLen, buf, bufsz) - 1;
						domainName = buf;
					}
					else domainLen /= sizeof(wchar_t);

					if (domainLen) {
						size_t newLoginLen = mir_wstrlen(login) + domainLen + 1;
						wchar_t *newLogin = (wchar_t*)alloca(newLoginLen * sizeof(wchar_t));

						wcsncpy(newLogin, domainName, domainLen);
						newLogin[domainLen] = '\\';
						mir_wstrcpy(newLogin + domainLen + 1, login);

						char* szChl = NtlmCreateResponseFromChallenge(hSecurity, NULL, newLogin, psw, http, complete);
						mir_free(szChl);
					}
				}
			}
		}
		else {
			if (SecIsValidHandle(&hNtlm->hClientContext))
				DeleteSecurityContext(&hNtlm->hClientContext);
			if (SecIsValidHandle(&hNtlm->hClientCredential))
				FreeCredentialsHandle(&hNtlm->hClientCredential);

			SEC_WINNT_AUTH_IDENTITY auth;

			if (login != NULL && login[0] != '\0') {
				memset(&auth, 0, sizeof(auth));

				NetlibLogf(NULL, "Security login requested, user: %S pssw: %s", login, psw ? "(exist)" : "(no psw)");

				const wchar_t* loginName = login;
				const wchar_t* domainName = wcschr(login, '\\');
				size_t domainLen = 0;
				size_t loginLen = mir_wstrlen(loginName);
				if (domainName != NULL) {
					loginName = domainName + 1;
					loginLen = mir_wstrlen(loginName);
					domainLen = domainName - login;
					domainName = login;
				}
				else if ((domainName = wcschr(login, '@')) != NULL) {
					loginName = login;
					loginLen = domainName - login;
					domainLen = mir_wstrlen(++domainName);
				}

				auth.User = (PWORD)loginName;
				auth.UserLength = (ULONG)loginLen;
				auth.Password = (PWORD)psw;
				auth.PasswordLength = (ULONG)mir_wstrlen(psw);
				auth.Domain = (PWORD)domainName;
				auth.DomainLength = (ULONG)domainLen;
				auth.Flags = SEC_WINNT_AUTH_IDENTITY_UNICODE;

				hNtlm->hasDomain = domainLen != 0;
			}

			SECURITY_STATUS sc = AcquireCredentialsHandle(NULL, szProvider,
				SECPKG_CRED_OUTBOUND, NULL, hNtlm->hasDomain ? &auth : NULL, NULL, NULL,
				&hNtlm->hClientCredential, &tokenExpiration);
			if (sc != SEC_E_OK) {
				ReportSecError(sc, __LINE__);
				return NULL;
			}
		}

		outputBufferDescriptor.cBuffers = 1;
		outputBufferDescriptor.pBuffers = &outputSecurityToken;
		outputBufferDescriptor.ulVersion = SECBUFFER_VERSION;
		outputSecurityToken.BufferType = SECBUFFER_TOKEN;
		outputSecurityToken.cbBuffer = hNtlm->cbMaxToken;
		outputSecurityToken.pvBuffer = alloca(outputSecurityToken.cbBuffer);

		SECURITY_STATUS sc = InitializeSecurityContext(&hNtlm->hClientCredential,
			hasChallenge ? &hNtlm->hClientContext : NULL,
			hNtlm->szPrincipal, isGSSAPI ? ISC_REQ_MUTUAL_AUTH | ISC_REQ_STREAM : 0, 0, SECURITY_NATIVE_DREP,
			hasChallenge ? &inputBufferDescriptor : NULL, 0, &hNtlm->hClientContext,
			&outputBufferDescriptor, &contextAttributes, &tokenExpiration);

		complete = (sc != SEC_I_COMPLETE_AND_CONTINUE && sc != SEC_I_CONTINUE_NEEDED);

		if (sc == SEC_I_COMPLETE_NEEDED || sc == SEC_I_COMPLETE_AND_CONTINUE)
			sc = CompleteAuthToken(&hNtlm->hClientContext, &outputBufferDescriptor);

		if (sc != SEC_E_OK && sc != SEC_I_CONTINUE_NEEDED) {
			ReportSecError(sc, __LINE__);
			return NULL;
		}

		szOutputToken = mir_base64_encode((PBYTE)outputSecurityToken.pvBuffer, outputSecurityToken.cbBuffer);
	}
	else {
		if (!login || !psw) return NULL;

		char *szLogin = mir_u2a(login);
		char *szPassw = mir_u2a(psw);

		size_t authLen = mir_strlen(szLogin) + mir_strlen(szPassw) + 5;
		char *szAuth = (char*)alloca(authLen);

		int len = mir_snprintf(szAuth, authLen, "%s:%s", szLogin, szPassw);
		szOutputToken = mir_base64_encode((BYTE*)szAuth, len);
		complete = true;

		mir_free(szPassw);
		mir_free(szLogin);
	}

	if (szOutputToken == NULL)
		return NULL;

	if (!http)
		return szOutputToken;

	ptrA szProvider(mir_u2a(hNtlm->szProvider));
	size_t resLen = mir_strlen(szOutputToken) + mir_strlen(szProvider) + 10;
	char *result = (char*)mir_alloc(resLen);
	mir_snprintf(result, resLen, "%s %s", szProvider, szOutputToken);
	mir_free(szOutputToken);
	return result;
}

///////////////////////////////////////////////////////////////////////////////

static INT_PTR InitSecurityProviderService(WPARAM, LPARAM lParam)
{
	HANDLE hSecurity = NetlibInitSecurityProvider((char*)lParam, NULL);
	return (INT_PTR)hSecurity;
}

static INT_PTR InitSecurityProviderService2(WPARAM, LPARAM lParam)
{
	NETLIBNTLMINIT2 *req = (NETLIBNTLMINIT2*)lParam;
	if (req == NULL || req->cbSize < sizeof(*req))
		return 0;

	if (req->flags & NNR_UNICODE)
		return (INT_PTR)NetlibInitSecurityProvider(req->szProviderName, req->szPrincipal);
	return (INT_PTR)NetlibInitSecurityProvider((char*)req->szProviderName, (char*)req->szPrincipal);
}

static INT_PTR DestroySecurityProviderService(WPARAM, LPARAM lParam)
{
	NetlibDestroySecurityProvider((HANDLE)lParam);
	return 0;
}

static INT_PTR NtlmCreateResponseService(WPARAM wParam, LPARAM lParam)
{
	NETLIBNTLMREQUEST *req = (NETLIBNTLMREQUEST*)lParam;
	if (req == NULL)
		return 0;

	unsigned complete = 0;
	char *response = NtlmCreateResponseFromChallenge((HANDLE)wParam, req->szChallenge, _A2T(req->userName), _A2T(req->password), false, complete);
	return (INT_PTR)response;
}

static INT_PTR NtlmCreateResponseService2(WPARAM wParam, LPARAM lParam)
{
	NETLIBNTLMREQUEST2 *req = (NETLIBNTLMREQUEST2*)lParam;
	if (req == NULL || req->cbSize < sizeof(*req))
		return 0;

	if (req->flags & NNR_UNICODE)
		return (INT_PTR)NtlmCreateResponseFromChallenge((HANDLE)wParam, req->szChallenge, req->szUserName, req->szPassword, false, req->complete);

	return (INT_PTR)NtlmCreateResponseFromChallenge((HANDLE)wParam, req->szChallenge, _A2T((char*)req->szUserName), _A2T((char*)req->szPassword), false, req->complete);
}

void NetlibSecurityInit(void)
{
	CreateServiceFunction(MS_NETLIB_INITSECURITYPROVIDER, InitSecurityProviderService);
	CreateServiceFunction(MS_NETLIB_INITSECURITYPROVIDER2, InitSecurityProviderService2);
	CreateServiceFunction(MS_NETLIB_DESTROYSECURITYPROVIDER, DestroySecurityProviderService);
	CreateServiceFunction(MS_NETLIB_NTLMCREATERESPONSE, NtlmCreateResponseService);
	CreateServiceFunction(MS_NETLIB_NTLMCREATERESPONSE2, NtlmCreateResponseService2);
}
