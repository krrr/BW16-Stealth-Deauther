<template>
  <div class="battery-indicator-wrapper" :title="tooltipText">
    <span class="battery-icon" v-html="currentSvg"></span>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import type { BatteryInfo } from '../utils/deviceStatus'
import batNone from '../assets/battery_none.svg?raw'
import bat0 from '../assets/battery_0.svg?raw'
import bat25 from '../assets/battery_25.svg?raw'
import bat50 from '../assets/battery_50.svg?raw'
import bat75 from '../assets/battery-75.svg?raw'
import bat100 from '../assets/battery_100.svg?raw'

const props = defineProps<{
  battery?: BatteryInfo
}>()

const activeBars = computed(() => {
  if (!props.battery || !props.battery.connected) return 0
  return Math.max(0, Math.min(4, props.battery.level))
})

const currentSvg = computed(() => {
  if (!props.battery || !props.battery.connected) {
    return batNone
  }
  switch (activeBars.value) {
    case 0:
      return bat0
    case 1:
      return bat25
    case 2:
      return bat50
    case 3:
      return bat75
    case 4:
    default:
      return bat100
  }
})

const tooltipText = computed(() => {
  if (!props.battery || !props.battery.connected) {
    return 'No Battery'
  }
  return `Voltage: ${props.battery.voltage.toFixed(2)}V | Percent: ${props.battery.percent}% (${activeBars.value}/4)`
})
</script>

<style scoped lang="scss">
.battery-indicator-wrapper {
}

.battery-icon {
  :deep(svg) {
    width: 24px;
    height: 24px;
    display: block;
  }
}
</style>
