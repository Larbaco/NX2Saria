import type { Inspection } from './inspection';

export function createMonitor(query: () => Promise<Inspection>, hide: () => boolean) {
  let stopped = false;
  let inFlight = false;
  let hidden = false;
  return {
    async check() {
      if (stopped || inFlight || hidden) return;
      inFlight = true;
      try {
        const state = await query();
        if (!stopped && state.game_running === true && state.hide_while_playing === true) {
          hidden = hide();
        }
      } finally { inFlight = false; }
    },
    stop() { stopped = true; return hidden; }
  };
}
