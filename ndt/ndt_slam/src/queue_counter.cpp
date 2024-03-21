class ForceSensorSubscriber(object):
    def __init__(self):
        self.data = []
        self.subscriber = rospy.Subscriber("/force_sensor_vel", Twist, self.callback)

    def callback(self, msg):
        self.data.append({"time": rospy.get_time(), "linear_z": msg.linear.z})

    def to_df(self):
        df = pd.DataFrame(self.data)
        if len(df) > 0:
            df = df.iloc[:-1]  # 最後の行を削除
        return df
