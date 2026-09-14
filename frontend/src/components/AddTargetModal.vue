<template>
  <dialog :open="open" @click.self="closeModal">
    <article class="add-target-modal-article">
      <header>
        <p style="margin: 0; font-weight: bold; font-size: 1.1rem;">
          Add Target Manually
        </p>
        <div class="flex1"></div>
        <button aria-label="Close" rel="prev" @click="closeModal"></button>
      </header>

      <form @submit.prevent="submitForm">
        <label style="margin-bottom: 0.75rem;">
          <span style="font-weight: 600; font-size: 0.9rem;">AP BSSID / MAC <span style="color:#e74c3c;">*</span></span>
          <input
            type="text"
            v-model="form.bssid"
            placeholder="e.g. 11:22:33:44:55:66"
            maxlength="17"
            required
            style="font-family: monospace;"
          />
        </label>

        <label style="margin-bottom: 0.75rem;">
          <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.2rem;">
            <span style="font-weight: 600; font-size: 0.9rem;">Target MAC <span style="color:#e74c3c;">*</span></span>
            <a href="#" @click.prevent="form.mac = 'FF:FF:FF:FF:FF:FF'" style="font-size: 0.8rem;">
              Broadcast (FF:FF:FF:FF:FF:FF)
            </a>
          </div>
          <input
            type="text"
            v-model="form.mac"
            placeholder="e.g. AA:BB:CC:DD:EE:FF or FF:FF:FF:FF:FF:FF"
            maxlength="17"
            required
            style="font-family: monospace;"
          />
        </label>

        <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 0.75rem; margin-bottom: 0.75rem;">
          <label style="margin-bottom: 0;">
            <span style="font-weight: 600; font-size: 0.9rem;">Channel <span style="color:#e74c3c;">*</span></span>
            <input
              type="number"
              v-model.number="form.channel"
              min="1"
              max="165"
              required
            />
          </label>
        </div>

        <label style="margin-bottom: 1rem;">
          <span style="font-weight: 600; font-size: 0.9rem;">AP SSID (For display only)</span>
          <input
            type="text"
            v-model="form.ssid"
            maxlength="32"
          />
        </label>

        <p v-if="errorMsg" role="alert" class="error" style="margin-bottom: 0.75rem; font-size: 0.85rem;">{{ errorMsg }}</p>

        <footer style="display: flex; justify-content: flex-end; gap: 0.5rem; margin-top: 0.5rem;">
          <button type="button" class="outline secondary" @click="closeModal">
            Cancel
          </button>
          <button type="submit" class="contrast">
            Add
          </button>
        </footer>
      </form>
    </article>
  </dialog>
</template>

<script setup lang="ts">
import { ref, reactive, watch, onMounted, onUnmounted } from 'vue'
import type { AttackTarget } from '../utils/attackPlan'

const props = withDefaults(defineProps<{
  open: boolean
  defaultChannel?: number | null
  existingTargets?: AttackTarget[]
}>(), {
  defaultChannel: 1,
  existingTargets: () => [],
})

const emit = defineEmits<{
  (e: 'close'): void
  (e: 'add', target: AttackTarget): void
}>()

const form = reactive({
  bssid: '',
  mac: '',
  ssid: '',
  channel: 1,
  rssi: -50 as number | null,
})
const errorMsg = ref('')

const resetForm = () => {
  form.bssid = ''
  form.mac = ''
  form.ssid = ''
  form.channel = props.defaultChannel || 1
  form.rssi = 0
  errorMsg.value = ''
}

watch(() => props.open, (isOpen) => {
  if (isOpen) {
    resetForm()
  }
})

const closeModal = () => {
  emit('close')
}

const isChannelValid = (ch: number): boolean => {
  if (ch >= 1 && ch <= 14) return true
  const valid5g = [
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128,
    132, 136, 140, 144, 149, 153, 157, 161, 165,
  ]
  return valid5g.includes(ch)
}

const normalizeMac = (input: string): string | null => {
  if (!input) return null
  const trimmed = input.trim()
  if (/^([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})$/.test(trimmed)) {
    return trimmed.replace(/-/g, ':').toUpperCase()
  }
  const rawHex = trimmed.replace(/[^0-9A-Fa-f]/g, '')
  if (rawHex.length === 12 && /^[0-9A-Fa-f]{12}$/.test(trimmed)) {
    return rawHex.match(/.{2}/g)!.join(':').toUpperCase()
  }
  return null
}

const submitForm = () => {
  errorMsg.value = ''
  const normBssid = normalizeMac(form.bssid)
  if (!normBssid) {
    errorMsg.value = 'Invalid AP BSSID. Please enter a valid MAC address (e.g. 11:22:33:44:55:66)'
    return
  }

  const normMac = normalizeMac(form.mac)
  if (!normMac) {
    errorMsg.value = 'Invalid Target MAC. Please enter a valid MAC address (e.g. AA:BB:CC:DD:EE:FF or FF:FF:FF:FF:FF:FF)'
    return
  }

  const ch = Number(form.channel)
  if (!isChannelValid(ch)) {
    errorMsg.value = 'Invalid Wi-Fi channel. Supported: 1-14 or 5GHz (36-165)'
    return
  }

  if (props.existingTargets.length >= 16) {
    errorMsg.value = 'Target limit reached (maximum 16 targets)'
    return
  }

  if (props.existingTargets.some(t => t.mac.toUpperCase() === normMac && t.bssid.toUpperCase() === normBssid)) {
    errorMsg.value = 'Target already exists in attack plan'
    return
  }

  const target: AttackTarget = {
    mac: normMac,
    bssid: normBssid,
    ssid: form.ssid.trim() || '(Manual)',
    channel: ch,
    rssi: form.rssi !== null && !isNaN(Number(form.rssi)) ? Number(form.rssi) : null,
    addedAt: new Date().toLocaleString('sv-SE'),
  }

  emit('add', target)
  closeModal()
}

const handleKeyDown = (e: KeyboardEvent) => {
  if (e.key === 'Escape' && props.open) {
    closeModal()
  }
}

onMounted(() => {
  window.addEventListener('keydown', handleKeyDown)
})

onUnmounted(() => {
  window.removeEventListener('keydown', handleKeyDown)
})
</script>

<style scoped lang="scss">
.add-target-modal-article {
  max-width: 520px;
  width: 95vw;
  margin: 0 auto;
}
</style>
