import express from 'express';
import * as configApi from './config-api';
import { getMetrics } from './metrics-api';
import { getStatus } from '../adapters/status-adapter';
import { handleDebugData } from './debug-api';

export function createRouter(): express.Router {
  const router = express.Router();

  // GET endpoints
  router.get('/api/state', configApi.handleGetState);
  router.get('/api/debug', handleDebugData);
  router.get('/api/metrics', async (_req, res) => {
    try {
      const metrics = await getMetrics();
      res.json(metrics);
    } catch (e: any) {
      res.status(500).json({ error: e.message });
    }
  });
  router.get('/api/dsp-status', async (_req, res) => {
    try {
      const result = await getStatus();
      if (!result.ok) {
        res.status(502).json({ error: result.error });
      } else {
        res.json(result.data);
      }
    } catch (e: any) {
      res.status(500).json({ error: e.message });
    }
  });
  router.get('/health', (_req, res) => {
    res.json({ status: 'ok' });
  });

  // POST endpoints (action-based routing to match PHP api.php)
  router.post('/api/:action', (req, res) => {
    const action = req.params.action;
    switch (action) {
      case 'save': configApi.handleSave(req, res); break;
      case 'save-staging': configApi.handleSaveStaging(req, res); break;
      case 'save-production': configApi.handleSaveProduction(req, res); break;
      case 'master': configApi.handleMaster(req, res); break;
      case 'apply': configApi.handleApply(req, res); break;
      case 'test-speaker': configApi.handleTestSpeaker(req, res); break;
      case 'stop-test-speaker': configApi.handleStopTestSpeaker(req, res); break;
      case 'test-pgm': configApi.handleTestPgm(req, res); break;
      case 'preset-save': configApi.handlePresetSave(req, res); break;
      case 'preset-select': configApi.handlePresetSelect(req, res); break;
      case 'preset-promote': configApi.handlePresetPromote(req, res); break;
      case 'preset-copy-production-to-staging': configApi.handlePresetCopyProductionToStaging(req, res); break;
      case 'preset-delete': configApi.handlePresetDelete(req, res); break;
      case 'preset-delete-all': configApi.handlePresetDeleteAll(req, res); break;
      case 'restore-defaults': configApi.handleRestoreDefaults(req, res); break;
      default:
        res.status(404).json({ error: `Unknown action: ${action}` });
    }
  });

  return router;
}
