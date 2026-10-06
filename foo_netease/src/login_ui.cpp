#include "stdafx.h"
#include "login_ui.h"

#include <helpers/DarkMode.h>

#include <algorithm>

#include "component_log.h"
#include "core/crypto.h"
#include "third_party/qrcodegen.hpp"
#include "resource.h"
#include "session.h"
#include "win_utf8.h"

namespace netease_ui {

namespace {

const UINT kMsgLoginState = WM_APP + 41;

// 把二维码渲染成 HBITMAP。调用方负责 DeleteObject。
HBITMAP render_qr_bitmap(const std::string & text, int module_px, int quiet_modules) {
	using qrcodegen::QrCode;
	const QrCode qr = QrCode::encodeText(text.c_str(), QrCode::Ecc::MEDIUM);
	const int modules = qr.getSize();
	const int total = (modules + quiet_modules * 2) * module_px;

	BITMAPINFO bi{};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = total;
	bi.bmiHeader.biHeight = -total; // 自上而下
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	void * bits = nullptr;
	HDC screen = GetDC(nullptr);
	HBITMAP bitmap = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	ReleaseDC(nullptr, screen);
	if (!bitmap || !bits) return nullptr;

	uint32_t * pixels = static_cast<uint32_t *>(bits);
	std::fill(pixels, pixels + static_cast<size_t>(total) * total, 0x00FFFFFFu); // 白底

	for (int y = 0; y < modules; ++y) {
		for (int x = 0; x < modules; ++x) {
			if (!qr.getModule(x, y)) continue;
			const int px0 = (x + quiet_modules) * module_px;
			const int py0 = (y + quiet_modules) * module_px;
			for (int dy = 0; dy < module_px; ++dy) {
				uint32_t * row = pixels + static_cast<size_t>(py0 + dy) * total + px0;
				for (int dx = 0; dx < module_px; ++dx) row[dx] = 0x00000000u;
			}
		}
	}
	return bitmap;
}

class QrLoginDialog : public CDialogImpl<QrLoginDialog> {
public:
	enum { IDD = IDD_NETEASE_LOGIN };

	BEGIN_MSG_MAP_EX(QrLoginDialog)
		MSG_WM_INITDIALOG(OnInitDialog)
		MSG_WM_DESTROY(OnDestroy)
		MESSAGE_HANDLER(kMsgLoginState, OnLoginState)
		COMMAND_ID_HANDLER_EX(IDC_LOGIN_REFRESH, OnRefresh)
		COMMAND_ID_HANDLER_EX(IDCANCEL, OnCancel)
	END_MSG_MAP()

private:
	BOOL OnInitDialog(CWindow, LPARAM) {
		SetWindowText(netease::to_wide("网易云音乐 · 扫码登录").c_str());
		SetDlgItemText(IDC_LOGIN_HINT, netease::to_wide(
			"1. 打开手机上的网易云音乐 App\n2. 点右上角「扫一扫」，扫描下方二维码\n3. 在手机上确认登录").c_str());
		SetDlgItemText(IDCANCEL, netease::to_wide("取消").c_str());
		SetDlgItemText(IDC_LOGIN_REFRESH, netease::to_wide("刷新二维码").c_str());

		m_dark.AddDialogWithControls(*this);

		HWND self = m_hWnd;
		netease::Session::instance().set_notify([self] {
			// 本回调已在主线程；用消息再走一圈可以避免在对话框初始化过程中重入。
			::PostMessage(self, kMsgLoginState, 0, 0);
		});
		netease::Session::instance().start_qr_login();
		refresh();
		return TRUE;
	}

	void OnDestroy() {
		netease::Session::instance().clear_notify();
		netease::Session::instance().cancel_qr_login();
		if (m_qr_bitmap) {
			DeleteObject(m_qr_bitmap);
			m_qr_bitmap = nullptr;
		}
	}

	LRESULT OnLoginState(UINT, WPARAM, LPARAM, BOOL &) {
		refresh();
		return 0;
	}

	void OnRefresh(UINT, int, CWindow) {
		netease::Session::instance().start_qr_login();
		refresh();
	}

	void OnCancel(UINT, int, CWindow) {
		netease::Session::instance().cancel_qr_login();
		EndDialog(IDCANCEL);
	}

	void refresh() {
		auto & session = netease::Session::instance();
		const netease::LoginState state = session.state();

		std::string status = std::string(netease::login_state_text(state));
		const std::string detail = session.status_detail();
		if (!detail.empty()) status += " —— " + detail;
		SetDlgItemText(IDC_LOGIN_STATUS, netease::to_wide(status).c_str());

		const std::string qr_text = session.qr_text();
		if (qr_text != m_rendered_text) {
			m_rendered_text = qr_text;
			if (m_qr_bitmap) {
				DeleteObject(m_qr_bitmap);
				m_qr_bitmap = nullptr;
			}
			if (!qr_text.empty()) {
				// 显示区约 300 像素见方；取 5 像素/模块时，45 模块的二维码约 265 像素，比较合适。
				const int module_px = 5;
				m_qr_bitmap = render_qr_bitmap(qr_text, module_px, 4);
			}
			HWND qr = GetDlgItem(IDC_LOGIN_QR);
			::SendMessage(qr, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(m_qr_bitmap));
		}

		if (state == netease::LoginState::LoggedIn) {
			EndDialog(IDOK);
		}
	}

	HBITMAP m_qr_bitmap = nullptr;
	std::string m_rendered_text;
	fb2k::CDarkModeHooks m_dark;
};



// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------

} // namespace

void show_login_dialog(fb2k::hwnd_t parent) {
	QrLoginDialog dialog;
	dialog.DoModal(reinterpret_cast<HWND>(parent));
}



} // namespace netease_ui

