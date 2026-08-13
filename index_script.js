(function () {
	const pathname = window.location.pathname || '/';
	const isRefill = pathname === '/refill' || pathname === '/refill/';
	const WS_PATH = '/ws';
	const CHANNEL = isRefill ? 'refill' : 'filter';
	const PAGE_TITLE = isRefill ? 'Refill Timer' : 'Filter Timer';
	document.querySelector('h1').textContent = PAGE_TITLE;

	const uptime = document.getElementById('uptime');
	const currentTime = document.getElementById('current_time');
	const wifiSsid = document.getElementById('wifi_ssid');
	const wifiIp = document.getElementById('wifi_ip');
	const connectionState = document.getElementById('connection_state');
	const manualDurationHValue = document.getElementById('manual_duration_h_value');
	const manualDurationMValue = document.getElementById('manual_duration_m_value');
	const manualDurationHDown = document.getElementById('manual_duration_h_down');
	const manualDurationHUp = document.getElementById('manual_duration_h_up');
	const manualDurationMDown = document.getElementById('manual_duration_m_down');
	const manualDurationMUp = document.getElementById('manual_duration_m_up');
	const timeLeft = document.getElementById('time_left');
	const switch_state = document.getElementById('switch_on_off');
	const runNowWrap = document.getElementById('run_now_wrap');
	const appPanel = document.getElementById('app_panel');
	const manualSection = document.getElementById('manual_section');
	const scheduleEnabled = document.getElementById('schedule_enabled');
	const scheduleStart = document.getElementById('schedule_start');
	const scheduleDurationHValue = document.getElementById('schedule_duration_h_value');
	const scheduleDurationMValue = document.getElementById('schedule_duration_m_value');
	const scheduleDurationHDown = document.getElementById('schedule_duration_h_down');
	const scheduleDurationHUp = document.getElementById('schedule_duration_h_up');
	const scheduleDurationMDown = document.getElementById('schedule_duration_m_down');
	const scheduleDurationMUp = document.getElementById('schedule_duration_m_up');
	const saveSchedule = document.getElementById('save_schedule');
	const scheduleStatus = document.getElementById('schedule_status');
	const dayGrid = document.getElementById('day_grid');

	const dayButtons = Array.from(dayGrid.querySelectorAll('.day-btn'));
	dayButtons.forEach((btn) => {
		btn.onclick = () => btn.classList.toggle('active');
	});

	let uptime_sec = 0, remaining_time_sec = 0;
	let local_epoch_sec = Math.floor(Date.now() / 1000) - (new Date().getTimezoneOffset() * 60);
	let manual_duration_sec = 5 * 3600;
	let uptime_timer = null;
	let reconnect_timer = null;
	let reconnect_attempt = 0;
	let ui_ready = false;
	let pending_manual_duration_sec = null;
	let pending_run_duration_sec = null;
	let run_command_sent_at = 0;
	let run_command_duration_sec = null;
	const pad0 = (n) => String(Math.floor(n)).padStart(2, '0');
	const clamp = (n, min, max) => Math.max(min, Math.min(max, n));

	const applyManualDurationFromState = (seconds) => {
		const parsed = parseInt(seconds, 10);
		if (!Number.isFinite(parsed) || parsed <= 0) return;
		manual_duration_sec = clamp(parsed, 1, 12 * 3600);
		pending_manual_duration_sec = null;
		renderManual();
	};

	const setManualDurationInputs = (totalSeconds) => {
		const seconds = Math.max(0, Math.floor(totalSeconds));
		manualDurationHValue.textContent = String(Math.floor(seconds / 3600));
		manualDurationMValue.textContent = String(Math.floor(seconds / 60) % 60);
	};

	const getManualDurationSeconds = () => {
		const hours = clamp(parseInt(manualDurationHValue.textContent) || 0, 0, 12);
		const minutes = clamp(parseInt(manualDurationMValue.textContent) || 0, 0, 59);
		return clamp(hours * 3600 + minutes * 60, 1, 12 * 3600);
	};

	const setScheduleDurationInputs = (totalMinutes) => {
		const minutes = Math.max(0, Math.floor(totalMinutes));
		scheduleDurationHValue.textContent = String(Math.floor(minutes / 60));
		scheduleDurationMValue.textContent = String(minutes % 60);
	};

	const getScheduleDurationMinutes = () => {
		const hours = clamp(parseInt(scheduleDurationHValue.textContent) || 0, 0, 12);
		const minutes = clamp(parseInt(scheduleDurationMValue.textContent) || 0, 0, 59);
		return hours * 60 + minutes;
	};

	const stepDuration = (valueNode, delta, min, max) => {
		const current = parseInt(valueNode.textContent) || 0;
		valueNode.textContent = String(clamp(current + delta, min, max));
	};

	const bindStepper = (button, step, onChange) => {
		let pressTimer = null;
		let repeatTimer = null;
		let repeating = false;
		button.style.touchAction = 'manipulation';

		const stop = () => {
			if (pressTimer !== null) {
				clearTimeout(pressTimer);
				pressTimer = null;
			}
			if (repeatTimer !== null) {
				clearInterval(repeatTimer);
				repeatTimer = null;
			}
			repeating = false;
		};

		button.addEventListener('pointerdown', (event) => {
			if (event.button !== 0) return;
			event.preventDefault();
			button.setPointerCapture(event.pointerId);
			step();
			onChange();
			pressTimer = setTimeout(() => {
				repeating = true;
				repeatTimer = setInterval(() => {
					step();
					onChange();
				}, 120);
			}, 300);
		});

		button.addEventListener('pointerup', stop);
		button.addEventListener('pointercancel', stop);
		button.addEventListener('pointerleave', stop);
		button.addEventListener('contextmenu', (event) => event.preventDefault());
		button.addEventListener('click', (event) => {
			if (repeating) event.preventDefault();
			stop();
		});
	};

	const durationToClock = (total) => {
		const sec = Math.max(0, Math.floor(total));
		const hr = Math.floor(sec / 3600);
		const min = Math.floor(sec / 60) % 60;
		const s = sec % 60;
		return `${hr}:${pad0(min)}:${pad0(s)}`;
	};

	const renderManual = () => {
		if (ui_ready) {
			setManualDurationInputs(manual_duration_sec);
		}
		manualDurationHDown.disabled = switch_state.checked;
		manualDurationHUp.disabled = switch_state.checked;
		manualDurationMDown.disabled = switch_state.checked;
		manualDurationMUp.disabled = switch_state.checked;
	};

	const renderTimeLeft = () => {
		timeLeft.textContent = durationToClock(remaining_time_sec);
	};

	const renderCurrentTime = () => {
		if (!Number.isFinite(local_epoch_sec) || local_epoch_sec < 0) {
			currentTime.textContent = '--';
			return;
		}
		const d = new Date(local_epoch_sec * 1000);
		const days = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
		currentTime.textContent = `${days[d.getUTCDay()]} ${pad0(d.getUTCHours())}:${pad0(d.getUTCMinutes())}:${pad0(d.getUTCSeconds())}`;
	};

	const setDisconnectedState = () => {
		connectionState.textContent = 'Disconnected';
		currentTime.textContent = 'disconnected';
		uptime.textContent = 'disconnected';
		timeLeft.textContent = '--';
	};

	const setConnectingState = () => {
		connectionState.textContent = 'Time left';
		currentTime.textContent = 'disconnected';
		uptime.textContent = 'disconnected';
		timeLeft.textContent = '--';
	};

	const setConnectedState = () => {
		connectionState.textContent = 'Time left';
	};

	const setDaysFromMask = (mask) => {
		dayButtons.forEach((b, i) => {
			b.classList.toggle('active', ((mask >> i) & 1) === 1);
		});
	};

	const toIntOrNull = (value) => {
		const n = parseInt(value, 10);
		return Number.isFinite(n) ? n : null;
	};

	const applyBackendState = (data) => {
		if (Object.prototype.hasOwnProperty.call(data, 'manual_duration_sec')) {
			applyManualDurationFromState(data['manual_duration_sec']);
		}
		if (wifiSsid && (Object.prototype.hasOwnProperty.call(data, 'ssid') || Object.prototype.hasOwnProperty.call(data, 'wifi_ssid'))) {
			wifiSsid.textContent = data['ssid'] || data['wifi_ssid'] || 'Connecting...';
		}
		if (wifiIp && (Object.prototype.hasOwnProperty.call(data, 'ip') || Object.prototype.hasOwnProperty.call(data, 'wifi_ip'))) {
			wifiIp.textContent = data['ip'] || data['wifi_ip'] || '0.0.0.0';
		}
		if (!ui_ready) {
			ui_ready = true;
			if (runNowWrap) {
				runNowWrap.style.visibility = 'visible';
				runNowWrap.classList.remove('init');
			}
			if (appPanel) appPanel.classList.remove('booting');
		}
		renderManual();
	};

	const getDaysMask = () => {
		let mask = 0;
		dayButtons.forEach((b, i) => {
			if (b.classList.contains('active')) mask |= (1 << i);
		});
		return mask;
	};

	const sendTimeSync = () => {
		const epochSec = Math.floor(Date.now() / 1000);
		const tzOffsetMin = -new Date().getTimezoneOffset();
		const msg = `sync_time:${epochSec}:${tzOffsetMin}`;
		if (websocket.readyState === WebSocket.OPEN) websocket.send(msg);
	};

	const start_uptime_timer = () => {
		if (uptime_timer !== null) return;
		uptime_timer = setInterval(() => {
			uptime_sec += 1000;
			const sec = uptime_sec / 1000;
			const min = sec / 60;
			const hr = sec / 3600;
			const d = hr / 24;
			uptime.innerHTML = `${Math.floor(d)} ${pad0(hr % 24)}:${pad0(min % 60)}:${pad0(sec % 60)}`;
			if (local_epoch_sec >= 0) {
				local_epoch_sec += 1;
				renderCurrentTime();
			}
			if (remaining_time_sec > 0 && switch_state.checked) {
				remaining_time_sec--;
				renderTimeLeft();
				if (remaining_time_sec === 0) {
					switch_state.checked = false;
					renderManual();
				}
			}
		}, 1000);
	};

	const stop_uptime_timer = () => {
		if (uptime_timer !== null) {
			clearInterval(uptime_timer);
			uptime_timer = null;
		}
	};

	const scheduleReconnect = () => {
		if (reconnect_timer !== null) return;
		setConnectingState();
		const delay = Math.min(1000 * Math.pow(2, reconnect_attempt), 10000);
		reconnect_attempt += 1;
		reconnect_timer = setTimeout(() => {
			reconnect_timer = null;
			connectWebSocket();
		}, delay);
	};

	const clearReconnectTimer = () => {
		if (reconnect_timer !== null) {
			clearTimeout(reconnect_timer);
			reconnect_timer = null;
		}
		reconnect_attempt = 0;
	};

	const wsProtocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
	let websocket = null;
	window.websocket = null;
	let channel_ready = false;

	const connectWebSocket = () => {
		if (websocket && (websocket.readyState === WebSocket.OPEN || websocket.readyState === WebSocket.CONNECTING)) {
			return;
		}
		websocket = new WebSocket(`${wsProtocol}//${window.location.host}${WS_PATH}`);
		window.websocket = websocket;
		const socket = websocket;

		socket.onopen = function () {
			console.log('WebSocket connected');
			clearReconnectTimer();
			setConnectedState();
			start_uptime_timer();
			channel_ready = false;
			socket.send('channel:' + CHANNEL);
			sendTimeSync();
		};

		socket.onclose = function () {
			if (websocket !== socket) return;
			channel_ready = false;
			console.warn('WebSocket disconnected');
			stop_uptime_timer();
			setDisconnectedState();
			scheduleReconnect();
		};

		socket.onerror = function () {
			console.warn('WebSocket error');
		};

		socket.onmessage = function (event) {
			try {
				if (event.data === 'channel_ready:' + CHANNEL) {
					channel_ready = true;
					websocket.send(CHANNEL + ':get_state:1');
					if (pending_run_duration_sec !== null) {
						websocket.send(CHANNEL + ':set_duration:' + pending_run_duration_sec);
						pending_run_duration_sec = null;
					}
					if (pending_manual_duration_sec !== null) {
						websocket.send(CHANNEL + ':set_manual_duration:' + pending_manual_duration_sec);
						pending_manual_duration_sec = null;
					}
					return;
				}
				if (event.data.startsWith('notice:')) {
					scheduleStatus.textContent = event.data.slice('notice:'.length);
					return;
				}

				if (!event.data.startsWith("{")) {
					const logMessage = event.data.replace("[log]:", "");
					console.log(logMessage);
					return;
				}
				console.log(`event.data: ${event.data}`);
				const data = JSON.parse(event.data);
				if (data['uptimeSeconds'] !== undefined || data['uptime'] !== undefined) {
					uptime_sec = parseInt(data['uptimeSeconds'] ?? data['uptime'], 10) * 1000;
				}

				const reportedRemaining = parseInt(data['remaining_time'], 10) || 0;
				if (run_command_duration_sec === 0 || reportedRemaining > 0 || Date.now() - run_command_sent_at > 1500) {
					remaining_time_sec = reportedRemaining;
					switch_state.checked = remaining_time_sec != 0;
				}
				renderTimeLeft();
				renderManual();
				applyBackendState(data);

				const hasScheduleFields =
					Object.prototype.hasOwnProperty.call(data, 'schedule_enabled') &&
					Object.prototype.hasOwnProperty.call(data, 'schedule_days') &&
					Object.prototype.hasOwnProperty.call(data, 'schedule_start') &&
					Object.prototype.hasOwnProperty.call(data, 'schedule_duration');

				if (hasScheduleFields) {
					const enabledNum = toIntOrNull(data['schedule_enabled']);
					const scheduleDays = toIntOrNull(data['schedule_days']);
					const startMin = toIntOrNull(data['schedule_start']);
					const durationMin = toIntOrNull(data['schedule_duration']);

					if (enabledNum !== null) scheduleEnabled.checked = enabledNum === 1;
					if (scheduleDays !== null) setDaysFromMask(scheduleDays & 0x7F);
					if (startMin !== null) {
						const hh = Math.floor(startMin / 60);
						const mm = startMin % 60;
						scheduleStart.value = `${pad0(hh)}:${pad0(mm)}`;
					}
					if (durationMin !== null) {
						setScheduleDurationInputs(durationMin);
					}
				}

				const localEpoch = toIntOrNull(data['local_epoch']);
				if (localEpoch !== null && localEpoch >= 0) {
					local_epoch_sec = localEpoch;
					renderCurrentTime();
				}
			} catch (error) {
				console.error(error);
			}
		};
	};

	const sendManualDurationToBackend = (seconds) => {
		const msg = CHANNEL + ':set_manual_duration:' + seconds;
		if (websocket.readyState === WebSocket.OPEN && channel_ready) {
			websocket.send(msg);
		} else {
			pending_manual_duration_sec = seconds;
		}
	};

	connectWebSocket();

	const websocket_send_msg = () => {
		const targetSec = remaining_time_sec > 0 ? 0 : getManualDurationSeconds();
		const msg = CHANNEL + ':set_duration:' + targetSec;
		if (websocket.readyState === WebSocket.OPEN && channel_ready) {
			console.log(`websocket.send: ${msg}`);
			run_command_sent_at = Date.now();
			run_command_duration_sec = targetSec;
			if (targetSec === 0) {
				remaining_time_sec = 0;
				renderTimeLeft();
			}
			websocket.send(msg);
			renderManual();
		} else {
			pending_run_duration_sec = targetSec;
			console.warn('WebSocket is not open; Run now command queued');
		}
	};

	document.getElementById('switch_on_off').onclick = websocket_send_msg;

	bindStepper(manualDurationHDown, () => stepDuration(manualDurationHValue, -1, 0, 12), () => {
		manual_duration_sec = getManualDurationSeconds();
		renderManual();
		sendManualDurationToBackend(manual_duration_sec);
	});
	bindStepper(manualDurationHUp, () => stepDuration(manualDurationHValue, 1, 0, 12), () => {
		manual_duration_sec = getManualDurationSeconds();
		renderManual();
		sendManualDurationToBackend(manual_duration_sec);
	});
	bindStepper(manualDurationMDown, () => stepDuration(manualDurationMValue, -1, 0, 59), () => {
		manual_duration_sec = getManualDurationSeconds();
		renderManual();
		sendManualDurationToBackend(manual_duration_sec);
	});
	bindStepper(manualDurationMUp, () => stepDuration(manualDurationMValue, 1, 0, 59), () => {
		manual_duration_sec = getManualDurationSeconds();
		renderManual();
		sendManualDurationToBackend(manual_duration_sec);
	});
	bindStepper(scheduleDurationHDown, () => stepDuration(scheduleDurationHValue, -1, 0, 12), () => {});
	bindStepper(scheduleDurationHUp, () => stepDuration(scheduleDurationHValue, 1, 0, 12), () => {});
	bindStepper(scheduleDurationMDown, () => stepDuration(scheduleDurationMValue, -1, 0, 59), () => {});
	bindStepper(scheduleDurationMUp, () => stepDuration(scheduleDurationMValue, 1, 0, 59), () => {});

	saveSchedule.onclick = () => {
		const daysMask = getDaysMask();
		const timeParts = (scheduleStart.value || '08:00').split(':');
		const startMin = (parseInt(timeParts[0]) || 0) * 60 + (parseInt(timeParts[1]) || 0);
		const durationMin = getScheduleDurationMinutes();
		const enabled = scheduleEnabled.checked ? 1 : 0;

		const msg = `set_schedule:${enabled}:${daysMask}:${startMin}:${durationMin}`;
		if (websocket.readyState === WebSocket.OPEN) {
			websocket.send(msg);
			scheduleStatus.textContent = 'Schedule saved';
		} else {
			scheduleStatus.textContent = 'WebSocket disconnected';
		}
	};

	renderManual();
	renderTimeLeft();
	renderCurrentTime();
	requestAnimationFrame(() => {
		if (runNowWrap) {
			runNowWrap.style.visibility = 'visible';
			runNowWrap.classList.remove('init');
		}
		if (appPanel) appPanel.classList.remove('booting');
	});
})();