<template>
  <section>
    <article>
      <header>
        <img src="../assets/attack.svg" />
        <h1>Attack</h1>
      </header>

      <!-- Config panel -->
      <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(12rem,1fr));gap:1rem;">
        <label>
          Attack Type
          <select v-model="config.type" :disabled="saving">
            <option value="deauth">Deauth</option>
          </select>
        </label>
        <label>
          Attack Interval (s)
          <input type="number" min="0" max="3600" step="0.1" v-model.number="config.intervalSec" :disabled="saving" />
        </label>
        <label>
          Power Save (AP)
          <span class="ap-status">
            <span class="ap-status-dot"
              :style="{ backgroundColor: apPowerSaveEnabled ? '#27ae60' : '#95a5a6' }"
            ></span>
            <strong>{{ apPowerSaveEnabled ? 'Enabled' : 'Disabled' }}</strong>
          </span>
        </label>
      </div>

      <div class="action-btn-div">
        <button @click="startAttack" :disabled="!canStart || saving" :aria-busy="saving" class="contrast">
          <span>{{ running ? 'Update & Restart' : 'Start Attack' }}</span>
        </button>
        <button v-if="running" @click="stopAttack" :disabled="saving" :aria-busy="saving" class="outline secondary">
          Stop
        </button>
        <button @click="openManualModal" :disabled="saving" class="outline secondary">
          Add Target
        </button>
        <span v-if="running" style="color:#e74c3c;font-weight:bold;">
          ● {{ attackTypeLabel }} running
          <small style="font-weight:normal;">
            · {{ status.rounds }} rounds / {{ status.packets }} packets sent
            <template v-if="nextCountdown !== null">· next in {{ nextCountdown }}s</template>
            · channel {{ apChannel }}
          </small>
        </span>
        <span v-else-if="plan.targets.length > 0" style="color:var(--pico-muted-color);font-size:0.9em;">
          {{ plan.targets.length }} targets configured, not started
        </span>
      </div>
      <p v-if="errorMsg" role="alert" class="error">{{ errorMsg }}</p>

      <!-- Target list -->
      <div style="overflow-x:auto;">
        <table>
          <thead>
            <tr>
              <th title="Client MAC">MAC</th>
              <th title="Associated hotspot">Hotspot</th>
              <th title="Channel">Ch</th>
              <th title="Signal strength when added">RSSI</th>
              <th title="Time added">Added</th>
              <th>Operations</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="t in plan.targets" :key="t.mac + '-' + t.bssid">
              <td>
                <span style="font-family:monospace;font-size:0.85em;cursor:pointer;" @click="openMacLookup(t.mac)">
                  {{ t.mac }}
                </span>
              </td>
              <td>
                {{ t.ssid || '(Hidden)' }}
                <small style="display:block;font-family:monospace;color:var(--pico-muted-color);">{{ t.bssid }}</small>
              </td>
              <td>{{ t.channel }}</td>
              <td>
                <span v-if="t.rssi" :style="{ color: rssiColor(t.rssi), fontWeight: 'bold' }">{{ t.rssi }} dBm</span>
                <span v-else style="color:var(--pico-muted-color)">—</span>
              </td>
              <td style="font-size:0.85em;">{{ t.addedAt || '—' }}</td>
              <td class="btn-col">
                <button @click="removeFromPlan(t)" :disabled="saving" class="outline btn-sm" style="color:#e74c3c;border-color:#e74c3c;">
                  Remove
                </button>
              </td>
            </tr>
            <tr v-if="plan.targets.length === 0">
              <td colspan="6" style="text-align:center;color:var(--pico-muted-color);padding: 1.5rem 1rem;">
                No targets — scan devices on the Scan page or click "Add Target" to specify manually
              </td>
            </tr>
          </tbody>
        </table>
      </div>

      <footer>
        <small>Target additions/removals are kept in the browser session only; the full plan is written to device flash and the attack starts when you click Start. Stop also writes to flash.</small>
      </footer>
    </article>

    <!-- Manual Target Modal -->
    <AddTargetModal
      :open="manualModalOpen"
      :default-channel="apChannel"
      :existing-targets="plan.targets"
      @close="manualModalOpen = false"
      @add="handleAddManualTarget"
    />
  </section>
</template>

<script setup lang="ts">
import { ref, reactive, computed, onMounted, onUnmounted } from 'vue'
import { message } from '../utils/message'
import AddTargetModal from '../components/AddTargetModal.vue'
import {
  loadPlan, savePlan, removeTarget, addTarget, hasPlan, defaultPlan,
  type AttackPlan, type AttackTarget, type AttackConfig,
} from '../utils/attackPlan'

interface AttackStatus {
  success: boolean
  enabled: boolean
  type: string
  interval_sec: number
  ap_powersave_enabled: boolean
  ap_saver_state: string
  rounds: number
  packets: number
  next_fire_ms: number
  ap_channel?: number
  targets: { mac: string, bssid: string, ssid: string, channel: number, rssi: number }[]
}

const plan = ref<AttackPlan>(hasPlan() ? loadPlan() : defaultPlan())
const config = ref<AttackConfig>(plan.value.config)

const saving = ref(false)
const errorMsg = ref('')
const running = ref(false)
const status = reactive({ rounds: 0, packets: 0, next_fire_ms: 0 })
const apPowerSaveEnabled = ref(false)
const apChannel = ref<number | null>(null)
let pollTimer: number | null = null

const manualModalOpen = ref(false)

const openManualModal = () => {
  manualModalOpen.value = true
}

const handleAddManualTarget = (target: AttackTarget) => {
  plan.value = addTarget(target)
  config.value = { ...plan.value.config }
  message.success(`Target added: ${target.mac}`)
}

const canStart = computed(() => plan.value.targets.length > 0 && !saving.value)

const attackTypeLabel = computed(() => config.value.type === 'deauth' ? 'Deauth' : config.value.type)

const nextCountdown = computed(() => {
  if (!running.value || status.next_fire_ms <= 0) return null
  return Math.max(0, Math.round(status.next_fire_ms / 1000))
})

const persistConfig = () => {
  plan.value.config = { ...config.value }
  savePlan(plan.value)
}

const removeFromPlan = (t: AttackTarget) => {
  plan.value = removeTarget(t.mac, t.bssid)
  config.value = { ...plan.value.config }
}

const startAttack = async () => {
  persistConfig()
  // Defensively clamp the interval range
  config.value.intervalSec = Math.min(3600, Math.max(1, Number(config.value.intervalSec) || 5))
  plan.value.config = { ...config.value }
  savePlan(plan.value)
  saving.value = true
  errorMsg.value = ''
  try {
    const r = await fetch('/api/attack/plan', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        enabled: true,
        type: config.value.type,
        interval_sec: config.value.intervalSec,
        targets: plan.value.targets.map(t => ({
          mac: t.mac,
          bssid: t.bssid,
          ssid: t.ssid,
          channel: t.channel,
          rssi: t.rssi,
        })),
      }),
    })
    const data = await r.json()
    if (data.success) {
      running.value = true
      message.success(`Attack started: ${plan.value.targets.length} targets, every ${config.value.intervalSec}s`)
    } else {
      errorMsg.value = data.message || 'Failed to start attack'
      message.error(data.message || 'Failed to start attack')
    }
  } catch (e) {
    errorMsg.value = 'Request failed: ' + (e as Error).message
  } finally {
    saving.value = false
  }
}

const stopAttack = async () => {
  persistConfig()
  saving.value = true
  errorMsg.value = ''
  try {
    const r = await fetch('/api/attack/plan', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        enabled: false,
        type: config.value.type,
        interval_sec: config.value.intervalSec,
        targets: plan.value.targets.map(t => ({
          mac: t.mac,
          bssid: t.bssid,
          ssid: t.ssid,
          channel: t.channel,
          rssi: t.rssi,
        })),
      }),
    })
    const data = await r.json()
    if (data.success) {
      running.value = false
      message.success('Attack stopped and plan saved to flash')
    } else {
      errorMsg.value = data.message || 'Failed to stop attack'
      message.error(data.message || 'Failed to stop attack')
    }
  } catch (e) {
    errorMsg.value = 'Request failed: ' + (e as Error).message
  } finally {
    saving.value = false
  }
}

const fetchStatus = async () => {
  
  try {
    const r = await fetch('/api/attack')
    const data: AttackStatus = await r.json()
    if (!data.success) return
    running.value = data.enabled
    status.rounds = data.rounds
    status.packets = data.packets
    status.next_fire_ms = data.next_fire_ms
    apPowerSaveEnabled.value = data.ap_powersave_enabled
    
    if (data.ap_channel !== undefined) apChannel.value = data.ap_channel
    // When no draft exists (e.g. new tab / first visit after reboot), init draft from the device's current plan
    if (!hasPlan()) {
      plan.value = {
        targets: (data.targets || []).map(t => ({
          ...t,
          lastSeen: '',
          addedAt: '',
        })),
        config: {
          type: data.type === 'unknown' ? 'deauth' : data.type,
          intervalSec: data.interval_sec,
        },
      }
      config.value = { ...plan.value.config }
      savePlan(plan.value)
    }
  } catch (e) {
    console.error(e)
  }
}

const openMacLookup = (mac: string) => {
  window.open(`https://maclookup.app/search/result?mac=${mac}`, 'maclookup', 'width=800,height=600')
}

const rssiColor = (rssi: number): string => {
  if (rssi >= -55) return '#27ae60'
  if (rssi >= -70) return '#f39c12'
  return '#e74c3c'
}

onMounted(() => {
  fetchStatus()
  pollTimer = window.setInterval(fetchStatus, 2000)
})

onUnmounted(() => {
  if (pollTimer !== null) {
    window.clearInterval(pollTimer)
  }
})
</script>

<style scoped lang="scss">
  .btn-col {
    white-space: nowrap;
  }
  .action-btn-div {
    display:flex;
    gap:0.75rem;
    align-items:center;
    flex-wrap:wrap;
    margin-bottom: 0.8rem;
  }
  .ap-status {
    margin-top: calc(var(--pico-spacing) * .25);
    display:flex;
    align-items:center;
    gap:0.4rem;
    padding: 9px 0;
  }
  .ap-status-dot {
    display: inline-block;
    width: 8px;
    height: 8px;
    border-radius: 50%;
  }
</style>
