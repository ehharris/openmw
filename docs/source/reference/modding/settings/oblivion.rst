Oblivion Settings
#################

These settings configure OpenMW's support for The Elder Scrolls IV: Oblivion (TES4) content files.
They live in the ``[Oblivion]`` section of ``settings.cfg`` and can also be controlled on the command line
(and in ``openmw.cfg``) with the equivalent options: ``oblivion``, ``oblivion-validate``,
``oblivion-strict-load-order`` and ``oblivion-verbose``.
When an option is set on the command line or in ``openmw.cfg``, it takes precedence over the
corresponding ``settings.cfg`` value.

.. omw-setting::
   :title: enable
   :type: boolean
   :range: true, false
   :default: true

   Enable Oblivion (TES4) content support. When disabled, configuring an Oblivion plugin/master
   (``.esm``/``.esp`` with a TES4 signature and an Oblivion HEDR version) is an error at startup.
   Does not affect Morrowind (TES3) content, which is always accepted.

.. omw-setting::
   :title: validate content
   :type: boolean
   :range: true, false
   :default: true

   Validate Oblivion content files at startup: the TES4 signature and HEDR version are checked,
   and every master dependency is checked against the configured content files and the data directories.
   Missing or misordered masters are reported as warnings by default.
   Equivalent command line option: ``oblivion-validate``.

.. omw-setting::
   :title: strict load order
   :type: boolean
   :range: true, false
   :default: false

   Treat missing or misordered Oblivion master files as fatal errors instead of warnings.
   Oblivion masters must be listed before the plugins that depend on them, both on the command line
   and in ``openmw.cfg``.
   Equivalent command line option: ``oblivion-strict-load-order``.

.. omw-setting::
   :title: verbose content loading
   :type: boolean
   :range: true, false
   :default: false

   Log detailed information about every Oblivion content file that is loaded, including its author,
   HEDR version and master dependencies.
   Equivalent command line option: ``oblivion-verbose``.
