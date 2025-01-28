import dataclasses
from typing import List

from . import Artifact, Job, Workflow
from .mangle import _get_workflows
from .parser import WorkflowConfigParser
from .runtime import RunConfig
from .settings import Settings
from .utils import Shell, Utils


class YamlGenerator:
    class Templates:
        TEMPLATE_PULL_REQUEST_0 = """\
# generated by praktika

name: {NAME}

on:
  {EVENT}:
    branches: [{BRANCHES}]

# Cancel the previous wf run in PRs.
concurrency:
  group: ${{{{{{{{ github.workflow }}}}}}}}-${{{{{{{{ github.ref }}}}}}}}
  cancel-in-progress: true

env:
  # Force the stdout and stderr streams to be unbuffered
  PYTHONUNBUFFERED: 1
  GH_TOKEN: ${{{{{{{{ github.token }}}}}}}}
{ENV_CHECKOUT_REFERENCE}

# Allow updating GH commit statuses and PR comments to post an actual job reports link
permissions: write-all

jobs:
{JOBS}\
"""
        TEMPLATE_ENV_CHECKOUT_REF_PR = """\
  USE_MERGE_COMMIT: ${{{{ vars.USE_MERGE_COMMIT || '0' }}}}
  CHECKOUT_REF: ${{{{ vars.USE_MERGE_COMMIT == '1' && github.event.pull_request.merge_commit_sha || github.head_ref }}}}
"""
        TEMPLATE_ENV_CHECKOUT_REF_PUSH = """\
  CHECKOUT_REF: ${{{{ github.head_ref }}}}
"""

        TEMPLATE_SCHEDULE = """\
# generated by praktika

name: {NAME}
on:
  schedule:{CRON_TEMPLATES}
  workflow_dispatch:

env:
  PYTHONUNBUFFERED: 1
{ENV_CHECKOUT_REFERENCE}

jobs:
{JOBS}\
"""

        TEMPLATE_CRON = """
    - cron: {CRON_SCHEDULE}\
"""

        TEMPLATE_DISPATCH_WORKFLOW = """\
# generated by praktika

name: {NAME}
on:
  workflow_dispatch:
    inputs:{DISPATCH_INPUTS}

env:
  PYTHONUNBUFFERED: 1
{ENV_CHECKOUT_REFERENCE}

jobs:
{JOBS}\
"""

        TEMPLATE_INPUT = """
      {NAME}:
        description: {DESCRIPTION}
        required: {IS_REQUIRED}
        default: {DEFAULT_VALUE}\
"""

        TEMPLATE_SECRET_CONFIG = """\
      {SECRET_NAME}:
        required: true
"""

        TEMPLATE_MATRIX = """
    strategy:
      fail-fast: false
      matrix:
        params: {PARAMS_LIST}\
"""

        TEMPLATE_JOB_0 = """
  {JOB_NAME_NORMALIZED}:
    runs-on: [{RUNS_ON}]
    needs: [{NEEDS}]{IF_EXPRESSION}
    name: "{JOB_NAME_GH}"
    outputs:
      data: ${{{{ steps.run.outputs.DATA }}}}
    steps:
      - name: Checkout code
        uses: actions/checkout@v4
        with:
          ref: ${{{{ env.CHECKOUT_REF }}}}
{JOB_ADDONS}
      - name: Prepare env script
        run: |
          rm -rf {INPUT_DIR} {OUTPUT_DIR} {TEMP_DIR}
          mkdir -p {TEMP_DIR} {INPUT_DIR} {OUTPUT_DIR}
          cat > {ENV_SETUP_SCRIPT} << 'ENV_SETUP_SCRIPT_EOF'
          export PYTHONPATH=./ci:.:{PYTHONPATH_EXTRA}
{SETUP_ENVS}
          cat > {WORKFLOW_STATUS_FILE} << 'EOF'
          ${{{{ toJson(needs) }}}}
          EOF
          ENV_SETUP_SCRIPT_EOF
{DOWNLOADS_GITHUB}
      - name: Run
        id: run
        run: |
          . {TEMP_DIR}/praktika_setup_env.sh
          set -o pipefail
          if command -v ts &> /dev/null; then
            python3 -m praktika run '{JOB_NAME}' --workflow "{WORKFLOW_NAME}" --ci |& ts '[%Y-%m-%d %H:%M:%S]' | tee {TEMP_DIR}/job.log
          else
            python3 -m praktika run '{JOB_NAME}' --workflow "{WORKFLOW_NAME}" --ci |& tee {TEMP_DIR}/job.log
          fi
{UPLOADS_GITHUB}\
"""

        TEMPLATE_SETUP_ENV_SECRETS = """\
          export {SECRET_NAME}=$(cat<<'EOF'
          ${{{{ secrets.{SECRET_NAME} }}}}
          EOF
          )\
"""

        TEMPLATE_SETUP_ENVS_INPUTS = """\
          cat > {WORKFLOW_INPUTS_FILE} << 'EOF'
          ${{{{ toJson(github.event.inputs) }}}}
          EOF\
"""

        TEMPLATE_SETUP_ENV_WF_CONFIG = """\
          cat > {WORKFLOW_CONFIG_FILE} << 'EOF'
          ${{{{ needs.{WORKFLOW_CONFIG_JOB_NAME}.outputs.data }}}}
          EOF\
"""

        TEMPLATE_PY_INSTALL = """
      - name: Set up Python
        uses: actions/setup-python@v5
        with:
          python-version: {PYTHON_VERSION}
"""

        TEMPLATE_PY_WITH_REQUIREMENTS = """
      - name: Install dependencies
        run: |
          sudo apt-get update && sudo apt install -y python3-pip
          # TODO: --break-system-packages? otherwise ubuntu's apt/apt-get complains
          {PYTHON} -m pip install --upgrade pip --break-system-packages
          {PIP} install -r {REQUIREMENT_PATH} --break-system-packages
"""

        TEMPLATE_GH_UPLOAD = """
      - name: Upload artifact {NAME}
        uses: actions/upload-artifact@v4
        with:
          name: {NAME}
          path: {PATH}
"""

        TEMPLATE_GH_DOWNLOAD = """
      - name: Download artifact {NAME}
        uses: actions/download-artifact@v4
        with:
          name: {NAME}
          path: {PATH}
"""

        TEMPLATE_IF_EXPRESSION = """
    if: ${{{{ !failure() && !cancelled() && !contains(fromJson(needs.{WORKFLOW_CONFIG_JOB_NAME}.outputs.data).cache_success_base64, '{JOB_NAME_BASE64}') }}}}\
"""

        TEMPLATE_IF_EXPRESSION_SKIPPED_OR_SUCCESS = """
    if: ${{ !failure() && !cancelled() }}\
"""

        TEMPLATE_IF_EXPRESSION_NOT_CANCELLED = """
    if: ${{ !cancelled() }}\
"""

    def __init__(self):
        self.py_workflows = []  # type: List[Workflow.Config]

    @classmethod
    def _get_workflow_file_name(cls, file_name):
        yaml_name = file_name.removesuffix(".py") + ".yml"
        return f"{Settings.WORKFLOW_PATH_PREFIX}/{Utils.normalize_string(yaml_name)}"

    def generate(self):
        print("---Start generating yaml pipelines---")
        files = []
        self.py_workflows = _get_workflows(_file_names_out=files)
        assert self.py_workflows and files
        for workflow_config, workflow_file_name in zip(self.py_workflows, files):
            print(f"Generate workflow [{workflow_config.name}]")
            parser = WorkflowConfigParser(workflow_config).parse()
            yaml_workflow_str = PullRequestPushYamlGen(parser).generate()
            with open(self._get_workflow_file_name(workflow_file_name), "w") as f:
                f.write(yaml_workflow_str)

        Shell.check("git add ./.github/workflows/*.yml")


class PullRequestPushYamlGen:
    def __init__(self, parser: WorkflowConfigParser):
        self.workflow_config = parser.workflow_yaml_config
        self.parser = parser

    def generate(self):
        job_items = []
        for i, job in enumerate(self.workflow_config.jobs):
            job_name_normalized = Utils.normalize_string(job.name)
            needs = ", ".join(map(Utils.normalize_string, job.needs))
            job_name = job.name
            job_addons = []
            for addon in job.addons:
                if addon.install_python:
                    job_addons.append(
                        YamlGenerator.Templates.TEMPLATE_PY_INSTALL.format(
                            PYTHON_VERSION=Settings.PYTHON_VERSION
                        )
                    )
                if addon.requirements_txt_path:
                    job_addons.append(
                        YamlGenerator.Templates.TEMPLATE_PY_WITH_REQUIREMENTS.format(
                            PYTHON=Settings.PYTHON_INTERPRETER,
                            PIP=Settings.PYTHON_PACKET_MANAGER,
                            PYTHON_VERSION=Settings.PYTHON_VERSION,
                            REQUIREMENT_PATH=addon.requirements_txt_path,
                        )
                    )
            uploads_github = []
            for artifact in job.artifacts_gh_provides:
                uploads_github.append(
                    YamlGenerator.Templates.TEMPLATE_GH_UPLOAD.format(
                        NAME=artifact.name, PATH=artifact.path
                    )
                )
            downloads_github = []
            for artifact in job.artifacts_gh_requires:
                downloads_github.append(
                    YamlGenerator.Templates.TEMPLATE_GH_DOWNLOAD.format(
                        NAME=artifact.name, PATH=Settings.INPUT_DIR
                    )
                )

            config_job_name_normalized = Utils.normalize_string(
                Settings.CI_CONFIG_JOB_NAME
            )

            if_expression = ""
            if (
                self.workflow_config.enable_cache
                and job_name_normalized != config_job_name_normalized
            ):
                if_expression = YamlGenerator.Templates.TEMPLATE_IF_EXPRESSION.format(
                    WORKFLOW_CONFIG_JOB_NAME=config_job_name_normalized,
                    JOB_NAME_BASE64=Utils.to_base64(job_name),
                )
            if job.run_unless_cancelled:
                if_expression = (
                    YamlGenerator.Templates.TEMPLATE_IF_EXPRESSION_NOT_CANCELLED
                )

            secrets_envs = []
            for secret in self.workflow_config.secret_names_gh:
                secrets_envs.append(
                    YamlGenerator.Templates.TEMPLATE_SETUP_ENV_SECRETS.format(
                        SECRET_NAME=secret
                    )
                )
            if self.workflow_config.event == Workflow.Event.DISPATCH:
                secrets_envs.append(
                    YamlGenerator.Templates.TEMPLATE_SETUP_ENVS_INPUTS.format(
                        WORKFLOW_INPUTS_FILE=Settings.WORKFLOW_INPUTS_FILE
                    )
                )
            if self.workflow_config.enable_cache:
                secrets_envs.append(
                    YamlGenerator.Templates.TEMPLATE_SETUP_ENV_WF_CONFIG.format(
                        WORKFLOW_CONFIG_FILE=RunConfig.file_name_static(
                            self.workflow_config.name
                        ),
                        WORKFLOW_CONFIG_JOB_NAME=config_job_name_normalized,
                    )
                )

            job_item = YamlGenerator.Templates.TEMPLATE_JOB_0.format(
                JOB_NAME_NORMALIZED=job_name_normalized,
                IF_EXPRESSION=if_expression,
                RUNS_ON=", ".join(job.runs_on),
                NEEDS=needs,
                JOB_NAME_GH=job_name.replace('"', '\\"'),
                JOB_NAME=job_name.replace(
                    "'", "'\\''"
                ),  # ' must be escaped so that yaml commands are properly parsed
                WORKFLOW_NAME=self.workflow_config.name,
                ENV_SETUP_SCRIPT=Settings.ENV_SETUP_SCRIPT,
                SETUP_ENVS="\n".join(secrets_envs),
                JOB_ADDONS="".join(job_addons),
                DOWNLOADS_GITHUB="\n".join(downloads_github),
                UPLOADS_GITHUB="\n".join(uploads_github),
                RUN_LOG=Settings.RUN_LOG,
                PYTHON=Settings.PYTHON_INTERPRETER,
                WORKFLOW_STATUS_FILE=Settings.WORKFLOW_STATUS_FILE,
                TEMP_DIR=Settings.TEMP_DIR,
                INPUT_DIR=Settings.INPUT_DIR,
                OUTPUT_DIR=Settings.OUTPUT_DIR,
                PYTHONPATH_EXTRA=Settings.PYTHONPATHS,
            )
            job_items.append(job_item)

        # for schedule workflows only
        cron_items = ""
        for cron_item in self.workflow_config.cron_schedules:
            cron_items += YamlGenerator.Templates.TEMPLATE_CRON.format(
                CRON_SCHEDULE=cron_item
            )

        # for dispatch workflows only
        dispatch_inputs = ""
        for input_item in self.workflow_config.dispatch_inputs:
            dispatch_inputs += YamlGenerator.Templates.TEMPLATE_INPUT.format(
                NAME=input_item.name,
                DESCRIPTION=input_item.description,
                IS_REQUIRED="true" if input_item.is_required else "false",
                DEFAULT_VALUE=input_item.default_value or "''",
            )

        if self.workflow_config.event in (
            Workflow.Event.PULL_REQUEST,
            Workflow.Event.PUSH,
        ):
            base_template = YamlGenerator.Templates.TEMPLATE_PULL_REQUEST_0
            format_kwargs = {
                "BRANCHES": ", ".join(
                    [f"'{branch}'" for branch in self.workflow_config.branches]
                ),
                "EVENT": self.workflow_config.event,
            }
            if self.workflow_config.event in (Workflow.Event.PULL_REQUEST,):
                ENV_CHECKOUT_REFERENCE = (
                    YamlGenerator.Templates.TEMPLATE_ENV_CHECKOUT_REF_PR
                )
            else:
                ENV_CHECKOUT_REFERENCE = (
                    YamlGenerator.Templates.TEMPLATE_ENV_CHECKOUT_REF_PUSH
                )
        elif self.workflow_config.event in (Workflow.Event.SCHEDULE,):
            base_template = YamlGenerator.Templates.TEMPLATE_SCHEDULE
            format_kwargs = {"CRON_TEMPLATES": cron_items}
            ENV_CHECKOUT_REFERENCE = (
                YamlGenerator.Templates.TEMPLATE_ENV_CHECKOUT_REF_PUSH
            )
        elif self.workflow_config.event in (Workflow.Event.DISPATCH,):
            base_template = YamlGenerator.Templates.TEMPLATE_DISPATCH_WORKFLOW
            format_kwargs = {"DISPATCH_INPUTS": dispatch_inputs}
            ENV_CHECKOUT_REFERENCE = (
                YamlGenerator.Templates.TEMPLATE_ENV_CHECKOUT_REF_PUSH
            )
        else:
            assert (
                False
            ), f"Invalid or Not implemented event [{self.workflow_config.event}]"

        template_1 = base_template.strip().format(
            NAME=self.workflow_config.name,
            JOBS="{}" * len(job_items),
            ENV_CHECKOUT_REFERENCE=ENV_CHECKOUT_REFERENCE,
            **format_kwargs,
        )
        res = template_1.format(*job_items)

        return res


@dataclasses.dataclass
class AuxConfig:
    # defines aux step to install dependencies
    addon: Job.Requirements
    # defines aux step(s) to upload GH artifacts
    uploads_gh: List[Artifact.Config]
    # defines aux step(s) to download GH artifacts
    downloads_gh: List[Artifact.Config]

    def get_aux_workflow_name(self):
        suffix = ""
        if self.addon.python_requirements_txt:
            suffix += "_py"
        for _ in self.uploads_gh:
            suffix += "_uplgh"
        for _ in self.downloads_gh:
            suffix += "_dnlgh"
        return f"{Settings.WORKFLOW_PATH_PREFIX}/aux_job{suffix}.yml"

    def get_aux_workflow_input(self):
        res = ""
        if self.addon.python_requirements_txt:
            res += f"      requirements_txt: {self.addon.python_requirements_txt}"
        return res


if __name__ == "__main__":
    WFS = [
        Workflow.Config(
            name="PR",
            event=Workflow.Event.PULL_REQUEST,
            jobs=[
                Job.Config(
                    name="Hello World",
                    runs_on=["foo"],
                    command="bar",
                    job_requirements=Job.Requirements(
                        python_requirements_txt="./requirement.txt"
                    ),
                )
            ],
            enable_cache=True,
        )
    ]
    YamlGenerator().generate(workflow_config=WFS)
