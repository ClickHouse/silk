import base64

from ci.settings.settings import PROJECT_NAME, PROJECT_SLUG
from praktika.infrastructure import Components, ImageBuilder, Storage, VPC
from praktika.infrastructure.cloud import CloudInfrastructure


CI_VPC_NAME = f"{PROJECT_SLUG}-ci"
_RUNTIME_BASE_VENV = "praktika-runtime"

# until published in pip
_PRAKTIKA_WHL = "https://praktika-artifacts-eu-north-1.s3.amazonaws.com/packages/praktika-0.1.2-py3-none-any.whl"
# until published in pip
_PRAKTIKA_CONTROLLER_WHL = "https://praktika-artifacts-eu-north-1.s3.amazonaws.com/packages/praktika_controller-0.1.1-py3-none-any.whl"


def _write_file_from_base64(path: str, content: str) -> str:
    payload = base64.b64encode(content.encode("utf-8")).decode("ascii")
    return f"printf '%s' '{payload}' | base64 -d > {path}"


def _controller_image_component(name: str):
    launcher = """#!/usr/bin/env bash
set -euo pipefail

TOKEN=$(curl -fsS -X PUT "http://169.254.169.254/latest/api/token" -H "X-aws-ec2-metadata-token-ttl-seconds: 60")
REGION=${AWS_DEFAULT_REGION:-$(curl -fsS -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/placement/region)}
INSTANCE_ID=${INSTANCE_ID:-$(curl -fsS -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/instance-id)}
PRAKTIKA_CONTROLLER_ROLE=${PRAKTIKA_CONTROLLER_ROLE:-$(curl -fsS -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/tags/instance/praktika_role || true)}
PRAKTIKA_CONTROLLER_QUEUE=${PRAKTIKA_CONTROLLER_QUEUE:-$(curl -fsS -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/tags/instance/praktika_queue || true)}
PRAKTIKA_PROJECT_SLUG=${PRAKTIKA_PROJECT_SLUG:-$(curl -fsS -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/tags/instance/praktika_project_slug || true)}
if [ -z "$PRAKTIKA_CONTROLLER_ROLE" ] || [ -z "$PRAKTIKA_CONTROLLER_QUEUE" ]; then
  echo "praktika_role or praktika_queue instance tag is unavailable" >&2
  exit 1
fi
export HOME=/root
export AWS_DEFAULT_REGION="$REGION"
export INSTANCE_ID="$INSTANCE_ID"
export PRAKTIKA_PROJECT_SLUG
export PRAKTIKA_CONTROLLER_ROLE
export PRAKTIKA_CONTROLLER_QUEUE
exec /usr/local/bin/praktika-controller
"""
    cloudwatch = """{
  "logs": {
    "logs_collected": {
      "files": {
        "collect_list": [
          {
            "file_path": "/var/log/praktika-controller.log",
            "log_group_name": "/praktika/controller",
            "log_stream_name": "{instance_id}",
            "timezone": "UTC"
          }
        ]
      }
    }
  }
}
"""
    unit = """[Unit]
Description=Praktika Controller
After=network.target docker.service

[Service]
Type=simple
Environment=HOME=/root
ExecStart=/usr/local/bin/praktika-controller-start
Restart=always
RestartSec=5
StandardOutput=append:/var/log/praktika-controller.log
StandardError=append:/var/log/praktika-controller.log

[Install]
WantedBy=multi-user.target
"""
    commands = [
        "dnf install -y python3 python3-pip python3.12 python3.12-pip git jq awscli",
        "dnf install -y amazon-cloudwatch-agent",
        "ln -sf /usr/bin/python3.12 /usr/local/bin/python3",
        "curl -fsSL https://cli.github.com/packages/rpm/gh-cli.repo -o /etc/yum.repos.d/gh-cli.repo",
        "dnf install -y gh",
        "dnf install -y docker",
        "usermod -aG docker ec2-user || true",
        "systemctl enable docker || true",
        "mkdir -p /opt/praktika /opt/praktika/work /opt/praktika/base-venvs",
        f"python3.12 -m venv /opt/praktika/base-venvs/{_RUNTIME_BASE_VENV}",
        f"/opt/praktika/base-venvs/{_RUNTIME_BASE_VENV}/bin/python -m pip install --upgrade pip setuptools wheel",
        f"/opt/praktika/base-venvs/{_RUNTIME_BASE_VENV}/bin/python -m pip install 'pytest>=7.0.0' {_PRAKTIKA_WHL}",
        f"python3.12 -m pip install --force-reinstall {_PRAKTIKA_CONTROLLER_WHL} --break-system-packages",
        "mkdir -p /etc/praktika",
        "touch /var/log/praktika-controller.log",
        "chmod 0644 /var/log/praktika-controller.log",
        _write_file_from_base64(
            "/usr/local/bin/praktika-controller-start", launcher
        ),
        "chmod 0755 /usr/local/bin/praktika-controller-start",
        _write_file_from_base64(
            "/etc/praktika/amazon-cloudwatch-agent.json", cloudwatch
        ),
        _write_file_from_base64(
            "/etc/systemd/system/praktika-controller.service", unit
        ),
        "systemctl daemon-reload || true",
    ]
    return {
        "name": name,
        "platform": "Linux",
        "description": "Install runner packages and bake the Praktika controller service",
        "commands": commands,
    }


def _image_builders():
    image_recipe_version = "1.0.1"
    return [
        ImageBuilder.Config(
            name="ci-arm64-image",
            image_recipe_name="ci-arm64-image-recipe",
            image_recipe_version=image_recipe_version,
            inline_components=[
                _controller_image_component("controller-image"),
            ],
            infrastructure_configuration_name="ci-arm64-imagebuilder-infra",
            instance_profile_name="arm-small-profile",
            instance_types=["t4g.small"],
            vpc_name=CI_VPC_NAME,
            security_group_names=[f"{CI_VPC_NAME}-sg"],
            distribution_configuration_name="ci-arm64-imagebuilder-dist",
            ami_name="ci-arm64-{{ imagebuilder:buildDate }}",
            ami_tags={"praktika_resource_tag": "controller", "arch": "arm64"},
            image_pipeline_name="ci-arm64-imagebuilder-pipeline",
        ),
        ImageBuilder.Config(
            name="ci-x86_64-image",
            image_recipe_name="ci-x86_64-image-recipe",
            image_recipe_version=image_recipe_version,
            inline_components=[
                _controller_image_component("controller-image"),
            ],
            infrastructure_configuration_name="ci-x86_64-imagebuilder-infra",
            instance_profile_name="amd-small-profile",
            instance_types=["t3.small"],
            vpc_name=CI_VPC_NAME,
            security_group_names=[f"{CI_VPC_NAME}-sg"],
            distribution_configuration_name="ci-x86_64-imagebuilder-dist",
            ami_name="ci-x86_64-{{ imagebuilder:buildDate }}",
            ami_tags={"praktika_resource_tag": "controller", "arch": "x86_64"},
            image_pipeline_name="ci-x86_64-imagebuilder-pipeline",
        ),
    ]


_GH_TOKEN_MINTER = Components.GitHubTokenMinter(
    repositories=[PROJECT_NAME],
)
_IMAGE_BUILDERS = _image_builders()
_IMAGE_BUILDERS_BY_NAME = {builder.name: builder for builder in _IMAGE_BUILDERS}

PROJECTS = [
    CloudInfrastructure.Config(
        name=PROJECT_NAME,
        min_praktika_version="0.1.2",
        vpcs=[
            VPC.Config(
                name=CI_VPC_NAME,
                subnets=[
                    VPC.Subnet(availability_zone="eu-north-1a"),
                ],
            )
        ],
        storages=[
            Storage.Config(
                name="artifacts-eu-north-1",
                retention_days=30,
                public=True,
            ),
        ],
        report_pages=[Components.report_page_config],
        image_builders=_IMAGE_BUILDERS,
        github_token_minters=[_GH_TOKEN_MINTER],
        orchestrator_pool=Components.OrchestratorPool(
            instance_type="t4g.small",
            vpc_name=CI_VPC_NAME,
            scaling=Components.OrchestratorPool.Scaling.Auto,
            size=0,
            max_size=50,
            capacity_reserve=1,
            image_builder=_IMAGE_BUILDERS_BY_NAME["ci-arm64-image"],
        ),
        runner_pools=[
            Components.RunnerPool(
                name="arm-small",
                instance_type="t4g.small",
                vpc_name=CI_VPC_NAME,
                scaling=Components.RunnerPool.Scaling.Auto,
                size=0,
                max_size=50,
                image_builder=_IMAGE_BUILDERS_BY_NAME["ci-arm64-image"],
            ),
            Components.RunnerPool(
                name="amd-small",
                instance_type="t3.small",
                vpc_name=CI_VPC_NAME,
                scaling=Components.RunnerPool.Scaling.Auto,
                size=0,
                max_size=50,
                image_builder=_IMAGE_BUILDERS_BY_NAME["ci-x86_64-image"],
            ),
            Components.RunnerPool(
                name="arm-medium",
                instance_type="c7g.4xlarge",
                vpc_name=CI_VPC_NAME,
                scaling=Components.RunnerPool.Scaling.Auto,
                size=0,
                max_size=50,
                volume_size_gb=30,
                image_builder=_IMAGE_BUILDERS_BY_NAME["ci-arm64-image"],
            ),
            Components.RunnerPool(
                name="amd-medium",
                instance_type="c7a.4xlarge",
                vpc_name=CI_VPC_NAME,
                scaling=Components.RunnerPool.Scaling.Auto,
                size=0,
                max_size=50,
                volume_size_gb=30,
                image_builder=_IMAGE_BUILDERS_BY_NAME["ci-x86_64-image"],
            ),
        ],
    )
]
