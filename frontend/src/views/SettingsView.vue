<template>
  <section>
    <article>
      <header>
        <img src="../assets/setting.svg" />
        <h1>Settings</h1>
      </header>

      <div>
        <h4 style="margin-bottom: 0.5rem">System Settings</h4>
        <p style="font-size: 0.9rem; margin-bottom: 0.75rem">
          Configure SoftAP / power etc. Saved to flash.
        </p>

        <label style="margin-bottom: 0.75rem">
          <span style="font-weight: 600; font-size: 0.9rem">SSID Name</span>
          <input
            type="text"
            v-model="apSsid"
            maxlength="32"
            required
            placeholder="e.g. BW16-SD"
            :disabled="savingSettings"
            style="margin-bottom: 0;"
          />
        </label>

        <label style="margin-bottom: 0.75rem">
          <span style="font-weight: 600; font-size: 0.9rem">Password</span>
          <div style="display: flex; gap: 0.5rem; margin-top: 0.25rem">
            <input
              v-model="apPass"
              minlength="8"
              maxlength="32"
              placeholder="8 - 32 characters"
              :disabled="savingSettings"
              style="margin-bottom: 0; flex: 1"
            />
          </div>
        </label>

        <label style="margin-bottom: 0.75rem">
          <span style="font-weight: 600; font-size: 0.9rem">Custom MAC Address</span>
          <input
            type="text"
            v-model="apMac"
            maxlength="17"
            placeholder="e.g. 02:12:34:56:78:9A (Leave empty for default)"
            :disabled="savingSettings"
            style="margin-bottom: 0; font-family: monospace;"
          />
        </label>

        <small style="font-size: 0.85rem; color: var(--pico-muted-color);">
          * SoftAP changes require a device reboot to take effect.
        </small>

        <label style="margin-bottom: 0.75rem; display: flex; align-items: center; gap: 0.5rem; cursor: pointer; margin-top: 1rem">
          <input
            type="checkbox"
            v-model="enableBeaconTimeSync"
            :disabled="savingSettings"
            style="margin-bottom: 0;"
            role="switch"
          />
          <span style="font-weight: 600; font-size: 0.9rem">Beacon Time Auto-Restore</span>
        </label>
        <small style="display: block; font-size: 0.85rem; color: var(--pico-muted-color); margin-top: -0.5rem; margin-bottom: 0.75rem;">
          When enabled, syncing time button samples surrounding Wi-Fi Beacon timestamps to Flash. On boot, RTC time is automatically restored from nearby Beacons.
          <span v-if="enableBeaconTimeSync && beaconRecordCount > 0" style="color: var(--pico-ins-color, #27ae60);"> ({{ beaconRecordCount }} AP records saved)</span>
        </small>
      </div>

      <hr style="margin: 1.5rem 0" />

      <div>
        <h4 style="margin-bottom: 0.5rem">AP Power Saving</h4>
        <p style="font-size: 0.9rem; margin-bottom: 0.75rem">
          Duty-cycle the SoftAP: it only stays on for a short window at the start of each cycle, then enters light sleep until the next cycle. It stays on while a device is connected.
        </p>

        <label style="margin-bottom: 0.75rem; display: flex; align-items: center; gap: 0.5rem; cursor: pointer">
          <input
            type="checkbox"
            v-model="apPowersaveEnable"
            :disabled="savingSettings"
            style="margin-bottom: 0;"
            role="switch"
          />
          <span style="font-weight: 600; font-size: 0.9rem">Enable AP Duty-Cycle Power Saving</span>
        </label>

        <div
          v-if="apPowersaveEnable"
          style="display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 0.75rem; margin-bottom: 0.75rem;"
        >
          <label>
            <span style="font-weight: 600; font-size: 0.9rem">Cycle Period (sec)</span>
            <input
              type="number"
              v-model.number="dutyPeriodSec"
              min="10"
              max="86400"
              required
              :disabled="savingSettings"
              style="margin-bottom: 0;"
            />
          </label>
          <label>
            <span style="font-weight: 600; font-size: 0.9rem">AP On Time (sec)</span>
            <input
              type="number"
              v-model.number="dutyOnSec"
              min="1"
              :max="dutyPeriodSec - 1"
              required
              :disabled="savingSettings"
              style="margin-bottom: 0;"
            />
          </label>
          <label>
            <span style="font-weight: 600; font-size: 0.9rem">Hold After Disconnect (sec)</span>
            <input
              type="number"
              v-model.number="clientHoldSec"
              min="0"
              max="3600"
              required
              :disabled="savingSettings"
              style="margin-bottom: 0;"
            />
          </label>
        </div>

        <label style="margin-bottom: 0.5rem; display: flex; align-items: center; gap: 0.5rem; cursor: pointer">
          <input
            type="checkbox"
            v-model="scheduleEnable"
            :disabled="savingSettings || !apPowersaveEnable"
            style="margin-bottom: 0;"
            role="switch"
          />
          <span style="font-weight: 600; font-size: 0.9rem">Enable Hour Schedule</span>
        </label>
        <small style="display: block; font-size: 0.85rem; color: var(--pico-muted-color); margin-bottom: 0.75rem;">
          The AP is only turned on during the selected hours. Requires a valid RTC time.
        </small>

        <div v-if="scheduleEnable" class="schedule-div flex-box-h">
          <div class="hour-grid">
            <button
              v-for="h in 24"
              :key="h"
              type="button"
              class="outline hour-chip"
              :class="{ 'hour-chip-on': isHourSelected(h - 1) }"
              :disabled="savingSettings"
              @click="toggleHour(h - 1)"
            >
              {{ h - 1 }}
            </button>
          </div>
          <div style="" class="schedule-btns flex-box-v">
            <button type="button" class="outline btn-sm" :disabled="savingSettings" @click="selectAllHours">Select all</button>
            <button type="button" class="outline btn-sm" :disabled="savingSettings" @click="clearAllHours">Clear</button>
          </div>
        </div>

        <small v-if="rtcInvalid" style="display: block; font-size: 0.85rem; color: var(--pico-del-color, #b42318);">
          * RTC time is not set. Hour schedule will be ignored until you sync time.
        </small>
      </div>

      <div style="display: flex; gap: 0.75rem; align-items: center; margin-top: 2rem; flex-wrap: wrap;">
        <button @click="saveSettings" :disabled="savingSettings" :aria-busy="savingSettings" style="width: auto; margin-bottom: 0">
          {{ savingSettings ? 'Saving...' : 'Save Settings' }}
        </button>
      </div>

      <hr style="margin: 1.5rem 0" />

      <div>
        <h4 style="margin-bottom: 0.5rem">OTA Firmware Update</h4>
        <div style="font-size: 0.9rem; margin-bottom: 0.5rem">
          <p style="margin-bottom: 0.25rem">Select the compiled firmware <code>OTA_All.bin</code> to upload and update the system.</p>
          <p v-if="otaSlot !== null" style="display: block; margin-top: 0.25rem;">
            <strong>Current Slot</strong>: {{ otaSlot }}
          </p>
        </div>
        <div style="display: flex; gap: 0.5rem; align-items: center; flex-wrap: wrap;">
          <input type="file" ref="otaFileInput" accept=".bin" style="margin-bottom: 0; flex: 1" :disabled="otaUploading" />
          <button @click="startOta" :disabled="otaUploading" :aria-busy="otaUploading" class="outline" style="margin-bottom: 0">
            {{ otaUploading ? 'Uploading...' : 'Start Update' }}
          </button>
        </div>
        <div v-if="otaProgress !== null" style="margin-top: 0.5rem">
          <progress v-if="otaProgress < 100" :value="otaProgress" max="100"></progress>
          <progress v-else></progress>
          <small style="font-size: 0.85rem">{{ otaStatusText }}</small>
        </div>
      </div>
      
      <hr style="margin: 1.5rem 0" />

      <div>
        <h4 style="margin-bottom: 0.5rem">System Control</h4>
        <p style="font-size: 0.9rem">
          Manually trigger a hardware reboot or deep sleep for the MCU.
        </p>
        <div style="display: flex; gap: 0.75rem; flex-wrap: wrap;">
          <button @click="rebootDevice" :disabled="rebooting || deepSleeping" :aria-busy="rebooting" class="outline secondary" style="width: auto; margin-bottom: 0">
            {{ rebooting ? 'Rebooting...' : 'Reboot Device' }}
          </button>
          <button @click="deepSleepDevice" :disabled="rebooting || deepSleeping" :aria-busy="deepSleeping" class="outline contrast danger" style="width: auto; margin-bottom: 0">
            {{ deepSleeping ? 'Entering Deep Sleep...' : 'Deep Sleep (Forever)' }}
          </button>
        </div>
      </div>

      <footer>
        <small>Configure software settings on this page</small>
      </footer>
    </article>
  </section>
</template>

<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { message } from '../utils/message'

const apSsid = ref('')
const apPass = ref('')
const apMac = ref('')
const enableBeaconTimeSync = ref(false)
const beaconRecordCount = ref(0)
const apPowersaveEnable = ref(false)
const dutyPeriodSec = ref(120)
const dutyOnSec = ref(10)
const clientHoldSec = ref(15)
const scheduleEnable = ref(false)
// 固件按 UTC 小时解释掩码；UI 上的 localHoursMask 按浏览器本地时区换算，
// 读取时 utc->local，保存时 local->utc
const scheduleHoursMask = ref(0)
const localHoursMask = ref(0)
const rtcTime = ref(0)
const savingSettings = ref(false)

// 本地相对 UTC 的偏移（分钟），东正西负；getTimezoneOffset() 是 UTC-本地，取反
const tzOffsetMinutes = -new Date().getTimezoneOffset()

// 小时粒度掩码换算，时区偏移按四舍五入取整小时（非整点偏移时区有 ±30min 边界误差）
const utcMaskToLocal = (mask: number): number => {
  const shift = Math.round(tzOffsetMinutes / 60)
  let out = 0
  for (let h = 0; h < 24; h++) {
    if ((mask >> h) & 1) {
      out |= 1 << ((((h + shift) % 24) + 24) % 24)
    }
  }
  return out
}

const localMaskToUtc = (mask: number): number => {
  const shift = Math.round(tzOffsetMinutes / 60)
  let out = 0
  for (let h = 0; h < 24; h++) {
    if ((mask >> h) & 1) {
      out |= 1 << ((((h - shift) % 24) + 24) % 24)
    }
  }
  return out
}

const rtcInvalid = computed(() => rtcTime.value <= 1600000000)

const otaFileInput = ref<HTMLInputElement | null>(null)
const otaUploading = ref(false)
const otaProgress = ref<number | null>(null)
const otaStatusText = ref('')
const rebooting = ref(false)
const deepSleeping = ref(false)
const otaSlot = ref<number | null>(null)

const fetchSettings = async () => {
  try {
    const res = await fetch('/api/settings')
    if (res.ok) {
      const data = await res.json()
      if (typeof data.ota_slot === 'number') {
        otaSlot.value = data.ota_slot
      }
      if (typeof data.ssid === 'string') {
        apSsid.value = data.ssid
      }
      if (typeof data.password === 'string') {
        apPass.value = data.password
      }
      if (typeof data.mac === 'string' && data.mac.length > 0) {
        apMac.value = data.mac
      }
      if (typeof data.enable_beacon_time_sync === 'boolean') {
        enableBeaconTimeSync.value = data.enable_beacon_time_sync
      }
      if (typeof data.beacon_record_count === 'number') {
        beaconRecordCount.value = data.beacon_record_count
      }
      if (typeof data.ap_powersave_enable === 'boolean') {
        apPowersaveEnable.value = data.ap_powersave_enable
      }
      if (typeof data.duty_period_sec === 'number') {
        dutyPeriodSec.value = data.duty_period_sec
      }
      if (typeof data.duty_on_sec === 'number') {
        dutyOnSec.value = data.duty_on_sec
      }
      if (typeof data.client_hold_sec === 'number') {
        clientHoldSec.value = data.client_hold_sec
      }
      if (typeof data.schedule_enable === 'boolean') {
        scheduleEnable.value = data.schedule_enable
      }
      if (typeof data.schedule_hours_mask === 'number') {
        scheduleHoursMask.value = data.schedule_hours_mask
        localHoursMask.value = utcMaskToLocal(scheduleHoursMask.value)
      }
    }
  } catch (err) {
    console.error('Failed to fetch settings:', err)
  }
  try {
    const res = await fetch('/api/status')
    if (res.ok) {
      const data = await res.json()
      if (typeof data.rtc_time === 'number') {
        rtcTime.value = data.rtc_time
      }
    }
  } catch (err) {
    console.error('Failed to fetch status:', err)
  }
}

onMounted(() => {
  fetchSettings()
})

const saveSettings = async () => {
  if (apPowersaveEnable.value && dutyOnSec.value >= dutyPeriodSec.value) {
    message.error('AP on time must be shorter than the cycle period')
    return
  }
  if (apPowersaveEnable.value && scheduleEnable.value && localHoursMask.value === 0) {
    message.error('Select at least one hour for the schedule')
    return
  }
  savingSettings.value = true
  try {
    const res = await fetch('/api/settings', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json'
      },
      body: JSON.stringify({
        ssid: apSsid.value,
        password: apPass.value,
        mac: apMac.value.trim(),
        enable_beacon_time_sync: enableBeaconTimeSync.value,
        ap_powersave_enable: apPowersaveEnable.value,
        duty_period_sec: dutyPeriodSec.value,
        duty_on_sec: dutyOnSec.value,
        client_hold_sec: clientHoldSec.value,
        schedule_enable: scheduleEnable.value,
        schedule_hours_mask: localMaskToUtc(localHoursMask.value)
      })
    })

    const data = await res.json()
    if (res.ok && data.success) {
      message.success('Settings saved successfully!')
    } else {
      message.error(data.message || 'Failed to save settings')
    }
  } catch (err) {
    message.error('Failed to send save request')
  } finally {
    savingSettings.value = false
  }
}

const isHourSelected = (hour: number) => {
  return (localHoursMask.value >> hour) & 1
}

const toggleHour = (hour: number) => {
  localHoursMask.value ^= 1 << hour
}

const selectAllHours = () => {
  localHoursMask.value = 0xffffff
}

const clearAllHours = () => {
  localHoursMask.value = 0
}

const rebootDevice = async () => {
  if (!confirm('Are you sure you want to reboot the device?')) {
    return
  }
  rebooting.value = true
  try {
    const res = await fetch('/api/reboot', { method: 'POST' })
    const data = await res.json()
    if (data.success) {
      message.success('Device is rebooting...')
    } else {
      message.error(data.message || 'Failed to reboot device')
      rebooting.value = false
    }
  } catch (err) {
    message.error('Failed to send reboot request')
    rebooting.value = false
  }
}

const deepSleepDevice = async () => {
  if (!confirm('Once in deep sleep mode, the device cannot be woken up automatically. Are you sure?')) {
    return
  }
  deepSleeping.value = true
  try {
    const res = await fetch('/api/deepsleep', { method: 'POST' })
    const data = await res.json()
    if (data.success) {
      message.success('Device is entering deep sleep...')
    } else {
      message.error(data.message || 'Failed to enter deep sleep')
      deepSleeping.value = false
    }
  } catch (err) {
    message.error('Failed to send deep sleep request')
    deepSleeping.value = false
  }
}

const startOta = () => {
  if (!otaFileInput.value || !otaFileInput.value.files || otaFileInput.value.files.length === 0) {
    message.error("Please select a firmware file (OTA_All.bin) first")
    return
  }
  const file = otaFileInput.value.files[0]
  otaUploading.value = true
  otaProgress.value = 0
  otaStatusText.value = 'Preparing upload...'

  const xhr = new XMLHttpRequest()
  xhr.open('POST', '/api/ota', true)
  xhr.setRequestHeader('Content-Type', 'application/octet-stream')

  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      otaProgress.value = Math.round((e.loaded / e.total) * 100)
      otaStatusText.value = `Uploaded ${otaProgress.value}%`
    }
  }

  xhr.onload = () => {
    otaUploading.value = false
    if (xhr.status === 200) {
      try {
        const response = JSON.parse(xhr.responseText)
        if (response.success) {
          message.success("Firmware updated successfully, device will reboot in 3 seconds!")
          otaStatusText.value = 'Update successful, device is rebooting...'
        } else {
          message.error(response.message || "Firmware update failed")
          otaStatusText.value = `Update failed: ${response.message}`
        }
      } catch (err) {
        message.error("Failed to parse server response")
        otaStatusText.value = 'Update failed: Failed to parse server response'
      }
    } else {
      message.error(`HTTP error ${xhr.status}`)
      otaStatusText.value = `Upload error (HTTP ${xhr.status})`
    }
  }

  xhr.onerror = () => {
    otaUploading.value = false
    message.error("Network connection error, update failed")
    otaStatusText.value = 'Network error'
  }

  xhr.send(file)
}
</script>

<style scoped>
.schedule-div {
  .hour-grid {
    display: grid;
    grid-template-columns: repeat(12, 1fr);
    gap: 0.2rem;
    width: 420px;
    border: var(--pico-border-width) solid var(--pico-muted-color);
    padding: 6px;
    border-radius: var(--pico-border-radius);
  }

  .hour-chip {
    aspect-ratio: 1;
    padding: 0;
    font-size: 0.72rem;
    line-height: 1;
    min-width: 0;
    margin-bottom: 0;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .hour-chip-on {
    background: var(--pico-primary);
    border-color: var(--pico-primary);
    color: var(--pico-primary-inverse, #fff);
  }

  .schedule-btns {
    display: flex;
    gap: 0.5rem;
    margin-left: 0.5rem;
    button {
      margin-bottom: 0;
    }
  }
}
</style>
