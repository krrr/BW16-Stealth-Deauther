<template>
  <section>
    <article>
      <header>
        <img src="../assets/dashboard.svg" />
        <h1>Status</h1>
      </header>
      <div style="display:flex;gap:0.5rem;align-items:center;flex-wrap:wrap;margin-bottom:1rem;">
        <span>Current Channel: </span>
        <div style="display:flex;gap:0.4rem;align-items:center;">
          <select v-model.number="selectedChannel" class="chan-sel">
            <optgroup label="2.4 GHz">
              <option v-for="ch in [1,2,3,4,5,6,7,8,9,10,11,12,13,14]" :key="ch" :value="ch">CH {{ ch }}</option>
            </optgroup>
            <optgroup label="5 GHz">
              <option v-for="ch in [36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165]" :key="ch" :value="ch">CH {{ ch }}</option>
            </optgroup>
          </select>
          <button @click="changeChannel" :disabled="switchingChannel || selectedChannel === apChannel" :aria-busy="switchingChannel" class="outline contrast btn-sm" style="margin-left: 6px">
            Switch
          </button>
        </div>
      </div>
      <p>
        Battery: 
        <span v-if="status.battery?.connected">
          {{ status.battery.voltage.toFixed(2) }}V ({{ status.battery.percent }}% · {{ status.battery.level }}/4)
        </span>
        <span v-else style="color:var(--pico-muted-color);">
          N/A
        </span>
      </p>
      <p v-if="formattedUptime">Uptime: {{ formattedUptime }}</p>
      <p v-if="freeHeap">Free Heap: {{ freeHeap }} KB</p>
      <p>
        RTC Time: <span>{{ rtcTime ? rtcTime.toLocaleString('sv-SE') : 'Not Set' }}</span>
        <span v-if="timeDiff !== null && timeDiff > 30" style="color:var(--del-color,#c0392b);font-size:0.9em;">
          (Offset <span>{{ Math.round(timeDiff) }}</span> s)
        </span>
        <button v-if="timeDiff !== null && timeDiff > 1" @click="syncTime" class="outline contrast btn-sm" style="margin-left:0.4rem">Sync Time</button>
      </p>
      <footer>
        <small v-if="compileDate">Firmware Build: {{ compileDate }}</small>
        <small style="float: right">by <a href="https://github.com/krrr" target="_blank">krrr</a></small>
      </footer>
    </article>
  </section>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, watch } from 'vue'
import { message } from '../utils/message'
import { useDeviceStatus } from '../utils/deviceStatus'

const { status, fetchDeviceStatus } = useDeviceStatus()

const selectedChannel = ref(-1)
const switchingChannel = ref(false)
const rtcTime = ref<Date | null>(null)
const timeDiff = ref<number | null>(null)

const apChannel = computed(() => status.value.ap_channel)
const freeHeap = computed(() => {
  if (status.value.free_heap !== undefined) {
    return (status.value.free_heap / 1024).toFixed(1)
  }
  return ''
})

const updateRtc = () => {
  if (status.value.rtc_time !== undefined) {
    // rtc_time returned by MCU is Unix timestamp in seconds, JS new Date() accepts milliseconds, so multiply by 1000.
    rtcTime.value = new Date(status.value.rtc_time * 1000)
    timeDiff.value = Math.abs(Math.floor(Date.now() / 1000) - status.value.rtc_time)
  } else {
    rtcTime.value = null
    timeDiff.value = null
  }
}

watch(
  () => status.value.rtc_time,
  () => {
    updateRtc()
  }
)

watch(
  () => status.value.ap_channel,
  (newCh) => {
    if (!switchingChannel.value && newCh > 0) {
      selectedChannel.value = newCh
    }
  },
  { immediate: true }
)

const changeChannel = async () => {
  if (selectedChannel.value === apChannel.value) return
  switchingChannel.value = true
  try {
    const r = await fetch('/api/change-channel', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ channel: selectedChannel.value })
    })
    const data = await r.json()
    if (data.success) {
      status.value.ap_channel = data.ap_channel
      selectedChannel.value = data.ap_channel
      message.success(`Channel successfully switched to ${data.ap_channel}`)
    } else {
      message.error(data.message || 'Failed to switch channel')
      selectedChannel.value = apChannel.value
    }
  } catch (e) {
    console.error('changeChannel error:', e)
    message.error('Network error, failed to switch channel')
    selectedChannel.value = apChannel.value
  } finally {
    switchingChannel.value = false
  }
}

const syncTime = async () => {
  try {
    const now = Math.floor(Date.now() / 1000)
    const r = await fetch(`/api/set-time?t=${now}`)
    const data = await r.json()
    if (data.success) {
      status.value.rtc_time = data.rtc_time
      rtcTime.value = new Date(data.rtc_time * 1000)
      timeDiff.value = 0
      message.success("Time synced successfully")
    }
  } catch (e) {
    console.error('syncTime error:', e)
  }
}

const formattedUptime = computed(() => {
  if (status.value.uptime < 0) return ''
  const h = Math.floor(status.value.uptime / 3600)
  const m = Math.floor((status.value.uptime % 3600) / 60)
  if (h > 0) return `${h}h ${m}m`
  return `${m}m`
})

const formattedCompileTime = (cDate: string, cTime: string): string => {
  if (!cDate || !cTime) return ''
  const d = new Date(`${cDate} ${cTime}`)
  if (isNaN(d.getTime())) return `${cDate} ${cTime}`
  return d.toLocaleString('sv-SE')
}

const compileDate = computed(() => {
  return formattedCompileTime(status.value.compile_date, status.value.compile_time)
})

onMounted(async () => {
  await fetchDeviceStatus()
})
</script>

<style scoped lang="scss">
.chan-sel {
  width: auto;
  margin: 0;
  font-size: 0.9rem;
  height: auto;
  padding-top: 3px;
  padding-bottom: 3px;
}
</style>