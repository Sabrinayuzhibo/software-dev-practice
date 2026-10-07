project('BackupSystem', 'cpp',
  version: '0.1.0',
  meson_version: '>=1.3.0',
  default_options: ['cpp_std=c++20', 'warning_level=3'])

qt6 = import('qt6')
qt_core = dependency('qt6', modules: ['Core'], version: '>=6.4', method: 'pkg-config')
qt_network = dependency('qt6', modules: ['Network'], method: 'pkg-config')
inc = include_directories('include')

protocol = static_library('backup-protocol',
  ['src/protocol/frame.cpp', 'src/core/io.cpp'],
  include_directories: inc,
  dependencies: qt_core)
protocol_dep = declare_dependency(
  link_with: protocol,
  include_directories: inc,
  dependencies: qt_core)

agent_moc = qt6.compile_moc(
  headers: 'include/backup/agent/agent_client.h',
  include_directories: inc,
  dependencies: [qt_core, qt_network])
agent_core = static_library('backup-agent-core',
  ['src/agent/agent_client.cpp', 'src/agent/channel.cpp',
   'src/agent/state.cpp', 'src/agent/history.cpp', 'src/agent/operations.cpp', agent_moc],
  include_directories: inc,
  dependencies: [protocol_dep, qt_network])
agent_dep = declare_dependency(
  link_with: agent_core,
  include_directories: inc,
  dependencies: [protocol_dep, qt_network])

server_moc = qt6.compile_moc(
  headers: 'include/backup/server/backup_server.h',
  include_directories: inc,
  dependencies: [qt_core, qt_network])
server_core = static_library('backup-server-core',
  ['src/server/backup_server.cpp', 'src/server/repository.cpp', server_moc],
  include_directories: inc,
  dependencies: [protocol_dep, qt_network])
server_dep = declare_dependency(
  link_with: server_core,
  include_directories: inc,
  dependencies: [protocol_dep, qt_network])

agent = executable('backup-agent',
  'apps/backup-agent/main.cpp',
  include_directories: inc,
  dependencies: agent_dep)
server = executable('backup-server',
  'apps/backup-server/main.cpp',
  include_directories: inc,
  dependencies: server_dep)

protocol_test = executable('protocol-test',
  'tests/protocol_test.cpp',
  include_directories: inc,
  dependencies: protocol_dep)
test('protocol-test', protocol_test)

core_test = executable('core-test', 'tests/core_test.cpp',
  include_directories: inc, dependencies: protocol_dep)
test('core-test', core_test)

python3 = find_program('python3')
integration_test = files('tests/server_integration.py')[0]
test('server-integration', python3,
  args: [integration_test, agent.full_path(), server.full_path()],
  timeout: 20)

test('backup-integration', python3,
  args: [files('tests/backup_integration.py')[0], agent.full_path(), server.full_path()],
  timeout: 120)

test('backup-faults', python3,
  args: [files('tests/backup_faults.py')[0], agent.full_path(), server.full_path()],
  timeout: 120)
test('backup-streaming', python3,
  args: [files('tests/backup_streaming.py')[0], agent.full_path(), server.full_path()],
  timeout: 180)

test('operation-history', python3,
  args: [files('tests/operation_history.py')[0], agent.full_path(), server.full_path()],
  timeout: 120)
